#ifndef AWG_BUNDLE_H
#define AWG_BUNDLE_H

#include <stddef.h>

#define AWG_APP_MAX_PROFILES 32
#define AWG_APP_MAX_STR 512
#define AWG_APP_NAME_MAX 64

typedef struct {
    char raw_name[AWG_APP_NAME_MAX];
    char name[AWG_APP_NAME_MAX];
    int enabled;
    int index;

    char interface_address[AWG_APP_MAX_STR];
    char interface_listen_port[32];
    char interface_private_key[AWG_APP_MAX_STR];
    char interface_mtu[32];
    char interface_dns[AWG_APP_MAX_STR];

    char jc[32];
    char jmin[32];
    char jmax[32];
    char s1[32];
    char s2[32];
    char s3[32];
    char s4[32];
    char h1[64];
    char h2[64];
    char h3[64];
    char h4[64];
    char i[5][AWG_APP_MAX_STR];

    char peer_public_key[AWG_APP_MAX_STR];
    char peer_preshared_key[AWG_APP_MAX_STR];
    char peer_allowed_ips[AWG_APP_MAX_STR];
    char peer_endpoint[AWG_APP_MAX_STR];
    char peer_persistent_keepalive[32];

    char routeros_public_key[AWG_APP_MAX_STR];
} awg_profile_t;

typedef struct {
    awg_profile_t profiles[AWG_APP_MAX_PROFILES];
    int count;
} awg_bundle_t;

int awg_bundle_load(const char *path, awg_bundle_t *bundle, char *err, size_t err_len);
int awg_bundle_validate(const awg_bundle_t *bundle, char *err, size_t err_len);
void awg_profile_managed_hash(const awg_profile_t *profile, char out[17]);

#endif
