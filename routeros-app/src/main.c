#include "awg_bundle.h"
#include "app_log.h"
#include "launcher.h"
#include "routeros_api.h"
#include "routeros_reconcile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v && v[0] ? atoi(v) : def;
}

static const char *env_str(const char *name, const char *def) {
    const char *v = getenv(name);
    return v && v[0] ? v : def;
}

static int is_app_token_literal(const char *value) {
    return strcmp(value, "[containerIP]") == 0 ||
           strcmp(value, "[containerInterface]") == 0 ||
           strcmp(value, "[routerIP]") == 0 ||
           strcmp(value, "[accessIP]") == 0 ||
           strcmp(value, "[accessPort]") == 0 ||
           strcmp(value, "[accessProto]") == 0;
}

static const char *env_effective(const char *name) {
    const char *v = getenv(name);
    if (!v || !v[0] || is_app_token_literal(v)) return NULL;
    return v;
}

static const char *env_app_str(const char *override_name, const char *bracket_name,
                               const char *plain_name, const char *normalized_name,
                               const char *def) {
    const char *v;
    if ((v = env_effective(override_name)) != NULL) return v;
    if ((v = env_effective(bracket_name)) != NULL) return v;
    if ((v = env_effective(plain_name)) != NULL) return v;
    if ((v = env_effective(normalized_name)) != NULL) return v;
    return def;
}

static void options_from_env(reconcile_options_t *opts) {
    memset(opts, 0, sizeof(*opts));
    opts->proxy_base_port = env_int("AWG_PROXY_BASE_PORT", 51820);
    opts->wg_fallback_base_port = env_int("AWG_WG_BASE_PORT", 42000);
    snprintf(opts->container_ip, sizeof(opts->container_ip), "%s",
             env_app_str("AWG_CONTAINER_IP", "[containerIP]", "containerIP",
                         "CONTAINER_IP", "172.18.0.2"));
    snprintf(opts->container_interface, sizeof(opts->container_interface), "%s",
             env_app_str("AWG_CONTAINER_INTERFACE", "[containerInterface]",
                         "containerInterface", "CONTAINER_INTERFACE", ""));
}

static void print_app_context(const reconcile_options_t *opts) {
    const char *router_ip = env_app_str("AWG_ROUTER_IP", "[routerIP]", "routerIP",
                                       "ROUTER_IP", "");
    const char *access_ip = env_app_str("AWG_ACCESS_IP", "[accessIP]", "accessIP",
                                       "ACCESS_IP", "");
    const char *access_port = env_app_str("AWG_ACCESS_PORT", "[accessPort]",
                                         "accessPort", "ACCESS_PORT", "");
    const char *access_proto = env_app_str("AWG_ACCESS_PROTO", "[accessProto]",
                                          "accessProto", "ACCESS_PROTO", "");

    app_log("INFO", "app network: container-ip=%s%s%s%s%s",
            opts->container_ip,
            opts->container_interface[0] ? " container-interface=" : "",
            opts->container_interface[0] ? opts->container_interface : "",
            router_ip[0] ? " router-ip=" : "",
            router_ip[0] ? router_ip : "");
    if (access_ip[0]) {
        app_log("INFO", "app access endpoint: %s://%s%s%s",
                access_proto[0] ? access_proto : "http",
                access_ip, access_port[0] ? ":" : "", access_port);
    }
}

static int append_host(char hosts[][128], int *count, int max, const char *host) {
    if (!host || !host[0] || is_app_token_literal(host)) return 0;
    for (int i = 0; i < *count; i++) {
        if (strcmp(hosts[i], host) == 0) return 0;
    }
    if (*count >= max) return -1;
    snprintf(hosts[*count], 128, "%s", host);
    (*count)++;
    return 0;
}

static int read_default_gateway(char *out, size_t out_len) {
    FILE *f = fopen("/proc/net/route", "rb");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char iface[64];
        unsigned int dest, gateway, flags;
        if (sscanf(line, "%63s %x %x %x", iface, &dest, &gateway, &flags) != 4)
            continue;
        (void)iface;
        (void)flags;
        if (dest != 0 || gateway == 0) continue;
        snprintf(out, out_len, "%u.%u.%u.%u",
                 gateway & 0xffu,
                 (gateway >> 8) & 0xffu,
                 (gateway >> 16) & 0xffu,
                 (gateway >> 24) & 0xffu);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static int connect_routeros(ros_api_t *api, const ros_credentials_t *creds,
                            char *err, size_t err_len) {
    char hosts[4][128];
    int host_count = 0;
    append_host(hosts, &host_count, 4, creds->host);
    if (creds->host_auto) {
        char gateway[128];
        if (read_default_gateway(gateway, sizeof(gateway)) == 0) {
            append_host(hosts, &host_count, 4, gateway);
        } else {
            app_log("WARN", "RouterOS API auto host: could not read container default gateway");
        }
        append_host(hosts, &host_count, 4, "172.18.0.1");
    }

    if (host_count == 0) {
        snprintf(err, err_len, "RouterOS API host is empty");
        return -1;
    }

    if (creds->host_auto)
        app_log("INFO", "RouterOS API auto host has %d candidate(s)", host_count);

    for (int i = 0; i < host_count; i++) {
        if (ros_api_connect(api, hosts[i], creds->port, creds->user, creds->password) == 0) {
            if (creds->host_auto && strcmp(hosts[i], creds->host) != 0)
                app_log("INFO", "RouterOS API auto host selected fallback %s", hosts[i]);
            return 0;
        }
        snprintf(err, err_len, "%s", api->last_error);
    }

    return -1;
}

static void log_worker_exit(int status) {
    if (WIFSIGNALED(status)) {
        app_log("ERROR", "proxy worker exited by signal %d", WTERMSIG(status));
    } else if (WIFEXITED(status)) {
        app_log("ERROR", "proxy worker exited with code %d", WEXITSTATUS(status));
    } else {
        app_log("ERROR", "proxy worker exited with status %d", status);
    }
}

static void log_enabled_profiles(const awg_bundle_t *bundle, const reconcile_options_t *opts) {
    int enabled = 0;
    for (int i = 0; i < bundle->count; i++) {
        const awg_profile_t *p = &bundle->profiles[i];
        if (!p->enabled) continue;
        enabled++;
        app_log("INFO", "AWG profile enabled: name=%s proxy-port=%d wg-port=%d",
                p->name, profile_proxy_port(p, opts), profile_wg_port(p, opts));
    }
    if (enabled == 0)
        app_log("WARN", "no enabled AWG profiles in config");
}

static void bundle_fingerprint(const awg_bundle_t *bundle, char *out, size_t out_len) {
    char buf[AWG_APP_MAX_PROFILES * 96];
    buf[0] = '\0';
    for (int i = 0; i < bundle->count; i++) {
        char h[17];
        char part[96];
        awg_profile_managed_hash(&bundle->profiles[i], h);
        snprintf(part, sizeof(part), "%s:%d:%s;", bundle->profiles[i].name,
                 bundle->profiles[i].enabled, h);
        strncat(buf, part, sizeof(buf) - strlen(buf) - 1);
    }
    snprintf(out, out_len, "%s", buf);
}

static void runtime_fingerprint(const awg_bundle_t *bundle, const reconcile_options_t *opts,
                                char *out, size_t out_len) {
    char bundle_fp[AWG_APP_MAX_PROFILES * 96];
    bundle_fingerprint(bundle, bundle_fp, sizeof(bundle_fp));
    snprintf(out, out_len, "proxy=%d;wg=%d;container=%s;%s",
             opts->proxy_base_port, opts->wg_fallback_base_port,
             opts->container_ip, bundle_fp);
}

static int connect_and_reconcile(awg_bundle_t *bundle, const reconcile_options_t *opts,
                                 const char *creds_path, char *err, size_t err_len) {
    ros_credentials_t creds;
    if (ros_credentials_load(creds_path, &creds, err, err_len) < 0)
        return -1;
    app_log("INFO", "RouterOS credentials loaded from %s: host=%s port=%d user=%s%s",
            creds_path, creds.host, creds.port, creds.user,
            creds.host_auto ? " mode=auto" : "");

    ros_api_t api;
    if (connect_routeros(&api, &creds, err, err_len) < 0) {
        return -1;
    }

    app_log("INFO", "RouterOS reconcile started");
    if (routeros_reconcile_bundle(&api, bundle, opts, err, err_len) < 0) {
        if (api.last_error[0]) {
            size_t used = strlen(err);
            snprintf(err + used, err_len > used ? err_len - used : 0, "%s%s",
                     used ? ": " : "", api.last_error);
        }
        ros_api_close(&api);
        return -1;
    }

    ros_api_close(&api);
    app_log("INFO", "RouterOS reconcile completed");
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    app_log("INFO", "awg-routeros-app starting");
    const char *config_path = env_str("AWG_CONFIG", "/etc/awg-proxy/awg-bundle.conf");
    app_log("INFO", "AWG config path: %s", config_path);

    char err[512];
    awg_bundle_t bundle;
    if (awg_bundle_load(config_path, &bundle, err, sizeof(err)) < 0) {
        app_log("FATAL", "%s", err);
        return 1;
    }
    app_log("INFO", "loaded %d AWG profile(s) from %s", bundle.count, config_path);

    reconcile_options_t opts;
    options_from_env(&opts);
    log_enabled_profiles(&bundle, &opts);
    print_app_context(&opts);
    const char *creds_path = env_str("AWG_ROUTEROS_CREDS", "/etc/awg-proxy/routeros-api.conf");

    if (connect_and_reconcile(&bundle, &opts, creds_path, err, sizeof(err)) < 0) {
        app_log("FATAL", "%s", err);
        return 1;
    }

    launcher_t launcher;
    if (launcher_start_profiles(&launcher, &bundle, &opts) < 0) {
        app_log("FATAL", "failed to start proxy workers");
        return 1;
    }

    char current_fp[AWG_APP_MAX_PROFILES * 96 + 256];
    runtime_fingerprint(&bundle, &opts, current_fp, sizeof(current_fp));
    int interval = env_int("AWG_RECONCILE_INTERVAL", 30);
    if (interval < 5) interval = 5;

    for (;;) {
        for (int i = 0; i < interval; i++) {
            int status = 0;
            int r = launcher_poll(&launcher, &status);
            if (r < 0 || r > 0) {
                if (r > 0) log_worker_exit(status);
                else app_log("ERROR", "failed to poll proxy workers");
                app_log("ERROR", "proxy worker exited, stopping app");
                launcher_stop(&launcher);
                return r < 0 ? 1 : status;
            }
            sleep(1);
        }

        awg_bundle_t fresh;
        if (awg_bundle_load(config_path, &fresh, err, sizeof(err)) < 0) {
            app_log("ERROR", "reconcile skipped: %s", err);
            continue;
        }

        reconcile_options_t fresh_opts;
        options_from_env(&fresh_opts);

        char fresh_fp[AWG_APP_MAX_PROFILES * 96 + 256];
        runtime_fingerprint(&fresh, &fresh_opts, fresh_fp, sizeof(fresh_fp));
        if (strcmp(current_fp, fresh_fp) != 0) {
            app_log("INFO", "AWG runtime inputs changed, restarting workers");
            launcher_stop(&launcher);
            bundle = fresh;
            opts = fresh_opts;
            log_enabled_profiles(&bundle, &opts);
            print_app_context(&opts);
            if (connect_and_reconcile(&bundle, &opts, creds_path, err, sizeof(err)) < 0) {
                app_log("FATAL", "%s", err);
                return 1;
            }
            if (launcher_start_profiles(&launcher, &bundle, &opts) < 0) {
                app_log("FATAL", "failed to restart proxy workers");
                return 1;
            }
            runtime_fingerprint(&bundle, &opts, current_fp, sizeof(current_fp));
        } else if (connect_and_reconcile(&bundle, &opts, creds_path, err, sizeof(err)) < 0) {
            app_log("ERROR", "reconcile failed: %s", err);
        }
    }
}
