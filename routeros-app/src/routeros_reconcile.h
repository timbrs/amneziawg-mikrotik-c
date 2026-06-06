#ifndef ROUTEROS_RECONCILE_H
#define ROUTEROS_RECONCILE_H

#include "awg_bundle.h"
#include "routeros_api.h"

#include <stddef.h>

typedef struct {
    char host[128];
    int port;
    char user[128];
    char password[256];
} ros_credentials_t;

typedef struct {
    int proxy_base_port;
    int wg_fallback_base_port;
    char container_ip[64];
    char container_interface[64];
} reconcile_options_t;

int ros_credentials_load(const char *path, ros_credentials_t *creds, char *err, size_t err_len);
int routeros_reconcile_bundle(ros_api_t *api, awg_bundle_t *bundle,
                              const reconcile_options_t *opts, char *err, size_t err_len);
int profile_proxy_port(const awg_profile_t *profile, const reconcile_options_t *opts);
int profile_wg_port(const awg_profile_t *profile, const reconcile_options_t *opts);
void profile_interface_name(const awg_profile_t *profile, char *out, size_t out_len);
void profile_comment(const awg_profile_t *profile, char *out, size_t out_len);

#endif
