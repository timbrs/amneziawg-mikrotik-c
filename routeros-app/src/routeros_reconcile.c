#include "routeros_reconcile.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void set_err(char *err, size_t err_len, const char *msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg);
}

static int is_app_token_literal(const char *value) {
    return strcmp(value, "[routerIP]") == 0 ||
           strcmp(value, "[containerIP]") == 0 ||
           strcmp(value, "[containerInterface]") == 0 ||
           strcmp(value, "[accessIP]") == 0 ||
           strcmp(value, "[accessPort]") == 0 ||
           strcmp(value, "[accessProto]") == 0;
}

static const char *env_effective(const char *name) {
    const char *v = getenv(name);
    if (!v || !v[0] || is_app_token_literal(v)) return NULL;
    return v;
}

static const char *env_first(const char *a, const char *b, const char *c) {
    const char *v;
    if ((v = env_effective(a)) != NULL) return v;
    if ((v = env_effective(b)) != NULL) return v;
    if ((v = env_effective(c)) != NULL) return v;
    return NULL;
}

static void endpoint_split(const char *endpoint, char *host, size_t host_len, char *port, size_t port_len) {
    const char *colon = strrchr(endpoint, ':');
    if (!colon) {
        snprintf(host, host_len, "%s", endpoint);
        snprintf(port, port_len, "0");
        return;
    }
    snprintf(host, host_len, "%.*s", (int)(colon - endpoint), endpoint);
    snprintf(port, port_len, "%s", colon + 1);
}

int ros_credentials_load(const char *path, ros_credentials_t *creds, char *err, size_t err_len) {
    memset(creds, 0, sizeof(*creds));
    creds->port = 8728;

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_len, "cannot open RouterOS credentials file");
        return -1;
    }

    char line_buf[1024];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        char *line = trim(line_buf);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);
        if (strcmp(key, "host") == 0) snprintf(creds->host, sizeof(creds->host), "%s", value);
        else if (strcmp(key, "port") == 0) creds->port = atoi(value);
        else if (strcmp(key, "user") == 0) snprintf(creds->user, sizeof(creds->user), "%s", value);
        else if (strcmp(key, "password") == 0) snprintf(creds->password, sizeof(creds->password), "%s", value);
    }
    fclose(f);

    if (strcmp(creds->host, "auto") == 0 || strcmp(creds->host, "[routerIP]") == 0) {
        const char *host = env_first("AWG_ROUTEROS_HOST", "[routerIP]", "ROUTER_IP");
        snprintf(creds->host, sizeof(creds->host), "%s", host ? host : "172.18.0.1");
    }

    if (!creds->host[0] || !creds->user[0] || !creds->password[0]) {
        set_err(err, err_len, "RouterOS credentials file must contain host, user and password");
        return -1;
    }
    return 0;
}

int profile_proxy_port(const awg_profile_t *profile, const reconcile_options_t *opts) {
    return opts->proxy_base_port + profile->index;
}

int profile_wg_port(const awg_profile_t *profile, const reconcile_options_t *opts) {
    return opts->wg_fallback_base_port + profile->index;
}

void profile_interface_name(const awg_profile_t *profile, char *out, size_t out_len) {
    snprintf(out, out_len, "wg-awg-%s", profile->name);
}

void profile_comment(const awg_profile_t *profile, char *out, size_t out_len) {
    char hash[17];
    awg_profile_managed_hash(profile, hash);
    snprintf(out, out_len, "awg-proxy:%s:%s", profile->name, hash);
}

static int ros_find_by_name(ros_api_t *api, const char *path, const char *name, ros_reply_t *reply) {
    char qname[256];
    snprintf(qname, sizeof(qname), "?name=%s", name);
    const char *cmd[] = { path, qname };
    return ros_api_command(api, cmd, 2, reply);
}

static int ros_find_address(ros_api_t *api, const char *iface, ros_reply_t *reply) {
    char qiface[256];
    snprintf(qiface, sizeof(qiface), "?interface=%s", iface);
    const char *cmd[] = { "/ip/address/print", qiface };
    return ros_api_command(api, cmd, 2, reply);
}

static int ros_find_peer(ros_api_t *api, const char *iface, ros_reply_t *reply) {
    char qiface[256];
    snprintf(qiface, sizeof(qiface), "?interface=%s", iface);
    const char *cmd[] = { "/interface/wireguard/peers/print", qiface };
    return ros_api_command(api, cmd, 2, reply);
}

static int ros_find_nat(ros_api_t *api, const char *comment_prefix, ros_reply_t *reply) {
    (void)comment_prefix;
    const char *cmd[] = { "/ip/firewall/nat/print" };
    return ros_api_command(api, cmd, 1, reply);
}

static int ensure_managed_collision_free(const ros_reply_t *reply, const char *name, char *err, size_t err_len) {
    const ros_sentence_t *item = ros_reply_first_re(reply);
    if (!item) return 0;
    const char *comment = ros_reply_find(item, "comment");
    if (!comment || !starts_with(comment, "awg-proxy:")) {
        snprintf(err, err_len, "RouterOS object name collision: %s exists without awg-proxy comment", name);
        return -1;
    }
    return 0;
}

static int ensure_wireguard(ros_api_t *api, const awg_profile_t *p, const reconcile_options_t *opts,
                            const char *iface, const char *comment, char *err, size_t err_len) {
    char listen_port[32], mtu[32], private_key[AWG_APP_MAX_STR], id_word[128], name_word[128], comment_word[256];
    snprintf(listen_port, sizeof(listen_port), "=listen-port=%d", profile_wg_port(p, opts));
    snprintf(mtu, sizeof(mtu), "=mtu=%s", p->interface_mtu[0] ? p->interface_mtu : "1420");
    snprintf(private_key, sizeof(private_key), "=private-key=%s", p->interface_private_key);
    snprintf(name_word, sizeof(name_word), "=name=%s", iface);
    snprintf(comment_word, sizeof(comment_word), "=comment=%s", comment);

    static ros_reply_t reply;
    if (ros_find_by_name(api, "/interface/wireguard/print", iface, &reply) < 0) return -1;
    if (ensure_managed_collision_free(&reply, iface, err, err_len) < 0) return -1;
    const ros_sentence_t *existing = ros_reply_first_re(&reply);
    if (!existing) {
        const char *cmd[] = { "/interface/wireguard/add", name_word, private_key, listen_port, mtu, comment_word, "=disabled=no" };
        return ros_api_command(api, cmd, 7, &reply);
    }

    const char *id = ros_reply_find(existing, ".id");
    if (!id) {
        set_err(err, err_len, "WireGuard print returned item without .id");
        return -1;
    }
    snprintf(id_word, sizeof(id_word), "=.id=%s", id);
    const char *cmd[] = { "/interface/wireguard/set", id_word, private_key, listen_port, mtu, comment_word, "=disabled=no" };
    return ros_api_command(api, cmd, 7, &reply);
}

static int read_wireguard_public_key(ros_api_t *api, const char *iface, char *out, size_t out_len,
                                     char *err, size_t err_len) {
    static ros_reply_t reply;
    if (ros_find_by_name(api, "/interface/wireguard/print", iface, &reply) < 0) return -1;
    const ros_sentence_t *existing = ros_reply_first_re(&reply);
    if (!existing) {
        snprintf(err, err_len, "WireGuard interface disappeared after reconcile: %s", iface);
        return -1;
    }
    const char *pub = ros_reply_find(existing, "public-key");
    if (!pub || !pub[0]) {
        snprintf(err, err_len, "WireGuard interface %s has no public-key in API response", iface);
        return -1;
    }
    snprintf(out, out_len, "%s", pub);
    return 0;
}

static int ensure_address(ros_api_t *api, const awg_profile_t *p, const char *iface, const char *comment,
                          char *err, size_t err_len) {
    char iface_word[128], addr_word[AWG_APP_MAX_STR], comment_word[256], id_word[128];
    snprintf(iface_word, sizeof(iface_word), "=interface=%s", iface);
    snprintf(addr_word, sizeof(addr_word), "=address=%s", p->interface_address);
    snprintf(comment_word, sizeof(comment_word), "=comment=%s", comment);

    static ros_reply_t reply;
    if (ros_find_address(api, iface, &reply) < 0) return -1;
    const ros_sentence_t *existing = ros_reply_first_re(&reply);
    if (!existing) {
        const char *cmd[] = { "/ip/address/add", iface_word, addr_word, comment_word };
        return ros_api_command(api, cmd, 4, &reply);
    }
    const char *ec = ros_reply_find(existing, "comment");
    if (!ec || !starts_with(ec, "awg-proxy:")) {
        snprintf(err, err_len, "IP address on %s exists without awg-proxy comment", iface);
        return -1;
    }
    const char *id = ros_reply_find(existing, ".id");
    if (!id) {
        set_err(err, err_len, "IP address print returned item without .id");
        return -1;
    }
    snprintf(id_word, sizeof(id_word), "=.id=%s", id);
    const char *cmd[] = { "/ip/address/set", id_word, addr_word, comment_word };
    return ros_api_command(api, cmd, 4, &reply);
}

static int ensure_peer(ros_api_t *api, const awg_profile_t *p, const reconcile_options_t *opts,
                       const char *iface, const char *comment, char *err, size_t err_len) {
    char endpoint_host[AWG_APP_MAX_STR], endpoint_port[32];
    endpoint_split(p->peer_endpoint, endpoint_host, sizeof(endpoint_host), endpoint_port, sizeof(endpoint_port));
    (void)endpoint_host;
    (void)endpoint_port;

    char iface_word[128], pub_word[AWG_APP_MAX_STR], psk_word[AWG_APP_MAX_STR], allowed_word[AWG_APP_MAX_STR];
    char endpoint_addr_word[128], endpoint_port_word[64], keepalive_word[64], comment_word[256], id_word[128];
    snprintf(iface_word, sizeof(iface_word), "=interface=%s", iface);
    snprintf(pub_word, sizeof(pub_word), "=public-key=%s", p->peer_public_key);
    snprintf(psk_word, sizeof(psk_word), "=preshared-key=%s", p->peer_preshared_key);
    snprintf(allowed_word, sizeof(allowed_word), "=allowed-address=%s", p->peer_allowed_ips);
    snprintf(endpoint_addr_word, sizeof(endpoint_addr_word), "=endpoint-address=%s", opts->container_ip);
    snprintf(endpoint_port_word, sizeof(endpoint_port_word), "=endpoint-port=%d", profile_proxy_port(p, opts));
    snprintf(keepalive_word, sizeof(keepalive_word), "=persistent-keepalive=%s",
             p->peer_persistent_keepalive[0] ? p->peer_persistent_keepalive : "25");
    snprintf(comment_word, sizeof(comment_word), "=comment=%s", comment);

    static ros_reply_t reply;
    if (ros_find_peer(api, iface, &reply) < 0) return -1;
    const ros_sentence_t *existing = ros_reply_first_re(&reply);
    const char *cmd_add_psk[] = { "/interface/wireguard/peers/add", iface_word, pub_word, psk_word, allowed_word,
                                  endpoint_addr_word, endpoint_port_word, keepalive_word, comment_word, "=disabled=no" };
    const char *cmd_add[] = { "/interface/wireguard/peers/add", iface_word, pub_word, allowed_word,
                              endpoint_addr_word, endpoint_port_word, keepalive_word, comment_word, "=disabled=no" };
    if (!existing) {
        if (p->peer_preshared_key[0]) return ros_api_command(api, cmd_add_psk, 10, &reply);
        return ros_api_command(api, cmd_add, 9, &reply);
    }

    const char *ec = ros_reply_find(existing, "comment");
    if (!ec || !starts_with(ec, "awg-proxy:")) {
        snprintf(err, err_len, "WireGuard peer on %s exists without awg-proxy comment", iface);
        return -1;
    }
    const char *id = ros_reply_find(existing, ".id");
    if (!id) {
        set_err(err, err_len, "WireGuard peer print returned item without .id");
        return -1;
    }
    snprintf(id_word, sizeof(id_word), "=.id=%s", id);

    const char *cmd_set_psk[] = { "/interface/wireguard/peers/set", id_word, pub_word, psk_word, allowed_word,
                                  endpoint_addr_word, endpoint_port_word, keepalive_word, comment_word, "=disabled=no" };
    const char *cmd_set[] = { "/interface/wireguard/peers/set", id_word, pub_word, allowed_word,
                              endpoint_addr_word, endpoint_port_word, keepalive_word, comment_word, "=disabled=no" };
    if (p->peer_preshared_key[0]) return ros_api_command(api, cmd_set_psk, 10, &reply);
    return ros_api_command(api, cmd_set, 9, &reply);
}

static int ensure_nat(ros_api_t *api, const awg_profile_t *p, const char *iface, const char *comment) {
    char out_word[128], comment_word[256];
    snprintf(out_word, sizeof(out_word), "=out-interface=%s", iface);
    snprintf(comment_word, sizeof(comment_word), "=comment=%s", comment);

    static ros_reply_t reply;
    char prefix[128];
    snprintf(prefix, sizeof(prefix), "awg-proxy:%s:", p->name);
    if (ros_find_nat(api, prefix, &reply) < 0) return -1;
    const ros_sentence_t *existing = NULL;
    for (int i = 0; i < reply.count; i++) {
        const ros_sentence_t *s = &reply.sentences[i];
        if (s->count == 0 || strcmp(s->words[0], "!re") != 0) continue;
        const char *c = ros_reply_find(s, "comment");
        if (c && starts_with(c, prefix)) {
            existing = s;
            break;
        }
    }
    if (!existing) {
        const char *cmd[] = { "/ip/firewall/nat/add", "=chain=srcnat", "=action=masquerade", out_word, comment_word };
        return ros_api_command(api, cmd, 5, &reply);
    }
    const char *id = ros_reply_find(existing, ".id");
    if (!id) return -1;
    char id_word[128];
    snprintf(id_word, sizeof(id_word), "=.id=%s", id);
    const char *cmd[] = { "/ip/firewall/nat/set", id_word, "=chain=srcnat", "=action=masquerade", out_word, comment_word };
    return ros_api_command(api, cmd, 6, &reply);
}

static int remove_sentence_id(ros_api_t *api, const char *path, const char *id) {
    char id_word[128];
    snprintf(id_word, sizeof(id_word), "=.id=%s", id);
    const char *cmd[] = { path, id_word };
    static ros_reply_t reply;
    return ros_api_command(api, cmd, 2, &reply);
}

static int comment_is_kept(const awg_bundle_t *bundle, const char *comment) {
    if (!comment || !starts_with(comment, "awg-proxy:")) return 1;
    for (int p = 0; p < bundle->count; p++) {
        if (!bundle->profiles[p].enabled) continue;
        char prefix[128];
        snprintf(prefix, sizeof(prefix), "awg-proxy:%s:", bundle->profiles[p].name);
        if (starts_with(comment, prefix)) return 1;
    }
    return 0;
}

static int cleanup_path_by_comment(ros_api_t *api, const awg_bundle_t *bundle,
                                   const char *print_path, const char *remove_path) {
    const char *cmd[] = { print_path };
    static ros_reply_t reply;
    if (ros_api_command(api, cmd, 1, &reply) < 0) return -1;
    for (int i = 0; i < reply.count; i++) {
        const ros_sentence_t *s = &reply.sentences[i];
        if (s->count == 0 || strcmp(s->words[0], "!re") != 0) continue;
        const char *comment = ros_reply_find(s, "comment");
        const char *id = ros_reply_find(s, ".id");
        if (!comment || !id || !starts_with(comment, "awg-proxy:")) continue;
        if (!comment_is_kept(bundle, comment) && remove_sentence_id(api, remove_path, id) < 0)
            return -1;
    }
    return 0;
}

static int cleanup_removed_profiles(ros_api_t *api, const awg_bundle_t *bundle) {
    if (cleanup_path_by_comment(api, bundle, "/interface/wireguard/peers/print",
                                "/interface/wireguard/peers/remove") < 0) return -1;
    if (cleanup_path_by_comment(api, bundle, "/ip/address/print",
                                "/ip/address/remove") < 0) return -1;
    if (cleanup_path_by_comment(api, bundle, "/ip/firewall/nat/print",
                                "/ip/firewall/nat/remove") < 0) return -1;
    if (cleanup_path_by_comment(api, bundle, "/interface/wireguard/print",
                                "/interface/wireguard/remove") < 0) return -1;
    return 0;
}

int routeros_reconcile_bundle(ros_api_t *api, awg_bundle_t *bundle,
                              const reconcile_options_t *opts, char *err, size_t err_len) {
    for (int i = 0; i < bundle->count; i++) {
        awg_profile_t *p = &bundle->profiles[i];
        if (!p->enabled) continue;

        char iface[96], comment[256];
        profile_interface_name(p, iface, sizeof(iface));
        profile_comment(p, comment, sizeof(comment));

        if (ensure_wireguard(api, p, opts, iface, comment, err, err_len) < 0) return -1;
        if (read_wireguard_public_key(api, iface, p->routeros_public_key,
                                      sizeof(p->routeros_public_key), err, err_len) < 0) return -1;
        if (ensure_address(api, p, iface, comment, err, err_len) < 0) return -1;
        if (ensure_peer(api, p, opts, iface, comment, err, err_len) < 0) return -1;
        if (ensure_nat(api, p, iface, comment) < 0) {
            set_err(err, err_len, "failed to reconcile NAT");
            return -1;
        }
    }

    if (cleanup_removed_profiles(api, bundle) < 0) {
        set_err(err, err_len, "failed to cleanup removed managed profiles");
        return -1;
    }
    return 0;
}
