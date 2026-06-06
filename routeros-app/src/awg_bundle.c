#include "awg_bundle.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SECTION_NONE,
    SECTION_INTERFACE,
    SECTION_PEER
} section_t;

static void set_err(char *err, size_t err_len, const char *msg) {
    if (!err || err_len == 0) return;
    snprintf(err, err_len, "%s", msg);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    return s;
}

static void copy_value(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    snprintf(dst, dst_len, "%s", src ? src : "");
}

static int streq_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void normalize_name(const char *raw, char *out, size_t out_len) {
    size_t n = 0;
    int last_dash = 0;
    for (const unsigned char *p = (const unsigned char *)raw; *p && n + 1 < out_len; p++) {
        char c = (char)tolower(*p);
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[n++] = c;
            last_dash = 0;
        } else if (c == '_' || c == '-' || c == '.') {
            if (!last_dash && n > 0) {
                out[n++] = '-';
                last_dash = 1;
            }
        }
    }
    while (n > 0 && out[n - 1] == '-') n--;
    if (n == 0) {
        snprintf(out, out_len, "default");
    } else {
        out[n] = '\0';
    }
}

static awg_profile_t *add_profile(awg_bundle_t *bundle, const char *raw_name, int enabled) {
    if (bundle->count >= AWG_APP_MAX_PROFILES) return NULL;
    awg_profile_t *p = &bundle->profiles[bundle->count];
    memset(p, 0, sizeof(*p));
    p->enabled = enabled;
    p->index = bundle->count;
    copy_value(p->raw_name, sizeof(p->raw_name), raw_name && raw_name[0] ? raw_name : "default");
    normalize_name(p->raw_name, p->name, sizeof(p->name));
    bundle->count++;
    return p;
}

static int parse_profile_marker(char *line, char *name, size_t name_len, int *enabled) {
    *enabled = 1;
    copy_value(name, name_len, "default");

    char *p = strstr(line, "#@awg-profile");
    if (!p) return -1;
    p += strlen("#@awg-profile");

    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        char *key = p;
        while (*p && *p != '=' && !isspace((unsigned char)*p)) p++;
        if (*p != '=') break;
        *p++ = '\0';
        char *val = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = '\0';
        key = trim(key);
        val = trim(val);
        if (streq_ci(key, "name")) {
            copy_value(name, name_len, val);
        } else if (streq_ci(key, "enabled")) {
            *enabled = !(streq_ci(val, "no") || streq_ci(val, "false") || strcmp(val, "0") == 0);
        }
    }
    return 0;
}

static void set_interface_key(awg_profile_t *p, const char *key, const char *value) {
    if (streq_ci(key, "Address")) copy_value(p->interface_address, sizeof(p->interface_address), value);
    else if (streq_ci(key, "ListenPort")) copy_value(p->interface_listen_port, sizeof(p->interface_listen_port), value);
    else if (streq_ci(key, "PrivateKey")) copy_value(p->interface_private_key, sizeof(p->interface_private_key), value);
    else if (streq_ci(key, "MTU")) copy_value(p->interface_mtu, sizeof(p->interface_mtu), value);
    else if (streq_ci(key, "DNS")) copy_value(p->interface_dns, sizeof(p->interface_dns), value);
    else if (streq_ci(key, "Jc")) copy_value(p->jc, sizeof(p->jc), value);
    else if (streq_ci(key, "Jmin")) copy_value(p->jmin, sizeof(p->jmin), value);
    else if (streq_ci(key, "Jmax")) copy_value(p->jmax, sizeof(p->jmax), value);
    else if (streq_ci(key, "S1")) copy_value(p->s1, sizeof(p->s1), value);
    else if (streq_ci(key, "S2")) copy_value(p->s2, sizeof(p->s2), value);
    else if (streq_ci(key, "S3")) copy_value(p->s3, sizeof(p->s3), value);
    else if (streq_ci(key, "S4")) copy_value(p->s4, sizeof(p->s4), value);
    else if (streq_ci(key, "H1")) copy_value(p->h1, sizeof(p->h1), value);
    else if (streq_ci(key, "H2")) copy_value(p->h2, sizeof(p->h2), value);
    else if (streq_ci(key, "H3")) copy_value(p->h3, sizeof(p->h3), value);
    else if (streq_ci(key, "H4")) copy_value(p->h4, sizeof(p->h4), value);
    else if (strlen(key) == 2 && (key[0] == 'I' || key[0] == 'i') && key[1] >= '1' && key[1] <= '5')
        copy_value(p->i[key[1] - '1'], sizeof(p->i[0]), value);
}

static void set_peer_key(awg_profile_t *p, const char *key, const char *value) {
    if (streq_ci(key, "PublicKey")) copy_value(p->peer_public_key, sizeof(p->peer_public_key), value);
    else if (streq_ci(key, "PresharedKey")) copy_value(p->peer_preshared_key, sizeof(p->peer_preshared_key), value);
    else if (streq_ci(key, "AllowedIPs")) copy_value(p->peer_allowed_ips, sizeof(p->peer_allowed_ips), value);
    else if (streq_ci(key, "Endpoint")) copy_value(p->peer_endpoint, sizeof(p->peer_endpoint), value);
    else if (streq_ci(key, "PersistentKeepalive")) copy_value(p->peer_persistent_keepalive, sizeof(p->peer_persistent_keepalive), value);
}

int awg_bundle_load(const char *path, awg_bundle_t *bundle, char *err, size_t err_len) {
    memset(bundle, 0, sizeof(*bundle));

    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, err_len, "cannot open AWG_CONFIG");
        return -1;
    }

    awg_profile_t *cur = NULL;
    section_t section = SECTION_NONE;
    char line_buf[2048];
    int line_no = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        line_no++;
        char *line = trim(line_buf);
        if (!line[0]) continue;

        if (strncmp(line, "#@awg-profile", 13) == 0) {
            char name[AWG_APP_NAME_MAX];
            int enabled;
            if (parse_profile_marker(line, name, sizeof(name), &enabled) < 0) {
                snprintf(err, err_len, "invalid profile marker at line %d", line_no);
                fclose(f);
                return -1;
            }
            cur = add_profile(bundle, name, enabled);
            if (!cur) {
                set_err(err, err_len, "too many profiles");
                fclose(f);
                return -1;
            }
            section = SECTION_NONE;
            continue;
        }

        if (line[0] == '#' || line[0] == ';') continue;

        if (strcmp(line, "[Interface]") == 0) {
            if (!cur) {
                cur = add_profile(bundle, "default", 1);
                if (!cur) {
                    set_err(err, err_len, "too many profiles");
                    fclose(f);
                    return -1;
                }
            }
            section = SECTION_INTERFACE;
            continue;
        }

        if (strcmp(line, "[Peer]") == 0) {
            if (!cur) {
                set_err(err, err_len, "[Peer] before [Interface]");
                fclose(f);
                return -1;
            }
            section = SECTION_PEER;
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(line);
        char *value = trim(eq + 1);

        if (!cur) {
            snprintf(err, err_len, "key before profile at line %d", line_no);
            fclose(f);
            return -1;
        }
        if (section == SECTION_INTERFACE) set_interface_key(cur, key, value);
        else if (section == SECTION_PEER) set_peer_key(cur, key, value);
    }

    fclose(f);
    return awg_bundle_validate(bundle, err, err_len);
}

static int missing(const char *s) {
    return !s || !s[0];
}

int awg_bundle_validate(const awg_bundle_t *bundle, char *err, size_t err_len) {
    if (bundle->count == 0) {
        set_err(err, err_len, "bundle contains no profiles");
        return -1;
    }

    for (int i = 0; i < bundle->count; i++) {
        const awg_profile_t *p = &bundle->profiles[i];
        for (int j = i + 1; j < bundle->count; j++) {
            if (strcmp(p->name, bundle->profiles[j].name) == 0) {
                snprintf(err, err_len, "duplicate profile name: %s", p->name);
                return -1;
            }
        }
        if (!p->enabled) continue;
        if (missing(p->interface_address) || missing(p->interface_private_key) ||
            missing(p->jc) || missing(p->jmin) || missing(p->jmax) ||
            missing(p->s1) || missing(p->s2) ||
            missing(p->h1) || missing(p->h2) || missing(p->h3) || missing(p->h4) ||
            missing(p->peer_public_key) || missing(p->peer_allowed_ips) || missing(p->peer_endpoint)) {
            snprintf(err, err_len, "profile %s is missing required fields", p->name);
            return -1;
        }
    }
    return 0;
}

static uint64_t fnv1a_update(uint64_t h, const char *s) {
    while (s && *s) {
        h ^= (unsigned char)*s++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

void awg_profile_managed_hash(const awg_profile_t *p, char out[17]) {
    uint64_t h = UINT64_C(1469598103934665603);
    h = fnv1a_update(h, p->name);
    h = fnv1a_update(h, p->interface_address);
    h = fnv1a_update(h, p->interface_listen_port);
    h = fnv1a_update(h, p->interface_private_key);
    h = fnv1a_update(h, p->interface_mtu);
    h = fnv1a_update(h, p->jc);
    h = fnv1a_update(h, p->jmin);
    h = fnv1a_update(h, p->jmax);
    h = fnv1a_update(h, p->s1);
    h = fnv1a_update(h, p->s2);
    h = fnv1a_update(h, p->s3);
    h = fnv1a_update(h, p->s4);
    h = fnv1a_update(h, p->h1);
    h = fnv1a_update(h, p->h2);
    h = fnv1a_update(h, p->h3);
    h = fnv1a_update(h, p->h4);
    h = fnv1a_update(h, p->peer_public_key);
    h = fnv1a_update(h, p->peer_preshared_key);
    h = fnv1a_update(h, p->peer_allowed_ips);
    h = fnv1a_update(h, p->peer_endpoint);
    h = fnv1a_update(h, p->peer_persistent_keepalive);
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}
