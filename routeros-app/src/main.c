#include "awg_bundle.h"
#include "launcher.h"
#include "routeros_api.h"
#include "routeros_reconcile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    fprintf(stderr, "app network: container-ip=%s", opts->container_ip);
    if (opts->container_interface[0])
        fprintf(stderr, " container-interface=%s", opts->container_interface);
    if (router_ip[0])
        fprintf(stderr, " router-ip=%s", router_ip);
    if (access_ip[0])
        fprintf(stderr, " access=%s://%s%s%s",
                access_proto[0] ? access_proto : "http",
                access_ip, access_port[0] ? ":" : "", access_port);
    fputc('\n', stderr);
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

    ros_api_t api;
    if (ros_api_connect(&api, creds.host, creds.port, creds.user, creds.password) < 0) {
        snprintf(err, err_len, "%s", api.last_error);
        return -1;
    }

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
    return 0;
}

int main(void) {
    const char *config_path = getenv("AWG_CONFIG");
    if (!config_path || !config_path[0]) {
        fprintf(stderr, "FATAL: AWG_CONFIG is not set\n");
        return 1;
    }

    char err[512];
    awg_bundle_t bundle;
    if (awg_bundle_load(config_path, &bundle, err, sizeof(err)) < 0) {
        fprintf(stderr, "FATAL: %s\n", err);
        return 1;
    }
    fprintf(stderr, "loaded %d AWG profile(s) from %s\n", bundle.count, config_path);

    reconcile_options_t opts;
    options_from_env(&opts);
    print_app_context(&opts);
    const char *creds_path = env_str("AWG_ROUTEROS_CREDS", "/etc/awg-proxy/routeros-api.conf");

    if (connect_and_reconcile(&bundle, &opts, creds_path, err, sizeof(err)) < 0) {
        fprintf(stderr, "FATAL: %s\n", err);
        return 1;
    }

    launcher_t launcher;
    if (launcher_start_profiles(&launcher, &bundle, &opts) < 0) {
        fprintf(stderr, "FATAL: failed to start proxy workers\n");
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
                fprintf(stderr, "proxy worker exited, stopping app\n");
                launcher_stop(&launcher);
                return r < 0 ? 1 : status;
            }
            sleep(1);
        }

        awg_bundle_t fresh;
        if (awg_bundle_load(config_path, &fresh, err, sizeof(err)) < 0) {
            fprintf(stderr, "reconcile skipped: %s\n", err);
            continue;
        }

        reconcile_options_t fresh_opts;
        options_from_env(&fresh_opts);

        char fresh_fp[AWG_APP_MAX_PROFILES * 96 + 256];
        runtime_fingerprint(&fresh, &fresh_opts, fresh_fp, sizeof(fresh_fp));
        if (strcmp(current_fp, fresh_fp) != 0) {
            fprintf(stderr, "AWG runtime inputs changed, restarting workers\n");
            launcher_stop(&launcher);
            bundle = fresh;
            opts = fresh_opts;
            print_app_context(&opts);
            if (connect_and_reconcile(&bundle, &opts, creds_path, err, sizeof(err)) < 0) {
                fprintf(stderr, "FATAL: %s\n", err);
                return 1;
            }
            if (launcher_start_profiles(&launcher, &bundle, &opts) < 0) {
                fprintf(stderr, "FATAL: failed to restart proxy workers\n");
                return 1;
            }
            runtime_fingerprint(&bundle, &opts, current_fp, sizeof(current_fp));
        } else if (connect_and_reconcile(&bundle, &opts, creds_path, err, sizeof(err)) < 0) {
            fprintf(stderr, "reconcile failed: %s\n", err);
        }
    }
}
