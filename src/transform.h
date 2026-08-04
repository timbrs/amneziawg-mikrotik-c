#ifndef AWG_TRANSFORM_H
#define AWG_TRANSFORM_H

#include <stdint.h>
#include <stddef.h>
#include "chacha20.h"

/* WireGuard message types (LE uint32 in first 4 bytes) */
#define WG_HANDSHAKE_INIT      1
#define WG_HANDSHAKE_RESPONSE  2
#define WG_COOKIE_REPLY        3
#define WG_TRANSPORT_DATA      4

/* WireGuard packet sizes */
#define WG_INIT_SIZE     148
#define WG_RESP_SIZE      92
#define WG_COOKIE_SIZE    64
#define WG_TRANSPORT_MIN  32
#define WG_TRANSPORT_HDR  16   /* type + receiver index + counter */

/* AWG 3.0 header protection: the padding doubles as the ChaCha20 nonce, so
 * every S value must be at least as long as that nonce. */
#define AWG_HP_MIN_PADDING CHACHA20_NONCE_SIZE

/* Shared fixed buffer sizes used by the proxy data paths */
#define AWG_PACKET_BUF_SIZE 1500
#define AWG_PACKET_HEADROOM 256

/* H range for v2 */
typedef struct {
    uint32_t min, max;
} hrange_t;

static inline uint32_t hrange_pick(const hrange_t *r, uint64_t rand_val) {
    if (r->min == r->max) return r->min;
    uint64_t span = (uint64_t)r->max - (uint64_t)r->min + 1u;
    return r->min + (uint32_t)(rand_val % span);
}

static inline int hrange_contains(const hrange_t *r, uint32_t v) {
    return v >= r->min && v <= r->max;
}

/* CPS segment kinds */
#define CPS_STATIC       'b'
#define CPS_RANDOM       'r'
#define CPS_TIMESTAMP    't'
#define CPS_COUNTER      'c'
#define CPS_RANDOM_CHARS 'C'
#define CPS_RANDOM_DIGITS 'D'

#define CPS_MAX_SEGMENTS 32
#define CPS_MAX_STATIC   1500

typedef struct {
    uint8_t kind;
    uint16_t size;        /* for r/rc/rd */
    uint16_t data_off;    /* offset into static_data for 'b' */
    uint16_t data_len;    /* length in static_data for 'b' */
} cps_segment_t;

typedef struct {
    cps_segment_t segs[CPS_MAX_SEGMENTS];
    uint8_t static_data[CPS_MAX_STATIC];
    int nseg;
    int static_used;
} cps_template_t;

/* Obfuscation profile — the subset of the config that differs between AWG
 * versions. The proxy keeps up to two (primary + v1 fallback) and can switch
 * the active one at runtime for site-to-site backward compatibility. All
 * profiles share the same S4 (padding stays 0 when fallback is enabled), so
 * switching never changes the packet buffer layout. */
typedef struct {
    int s1, s2, s3, s4;
    hrange_t h1, h2, h3, h4;
    cps_template_t *cps[5]; /* I1-I5, NULL if not configured */
    /* Derived (filled by config_compute_profile) */
    uint32_t h4_fixed;
    int h4_noop;
    int init_total;
    int resp_total;
    int cookie_total;
    int transport_size_ambiguous;
} awg_profile_t;

/* Config struct */
#define AWG_MAX_SERVER_PEERS 256

typedef struct {
    int jc, jmin, jmax;
    int s1, s2, s3, s4;
    hrange_t h1, h2, h3, h4;

    cps_template_t *cps[5]; /* I1-I5, NULL if not configured */

    /* AWG 3.0 header protection. The key is shared with the peer (server-side
     * HeaderProtectionKey); empty key = AWG 2.0 behaviour, nothing encrypted. */
    uint8_t hp_key[CHACHA20_KEY_SIZE];
    int has_hp;

    uint8_t server_pub[32];
    uint8_t client_pub[32];
    uint8_t server_peer_pubs[AWG_MAX_SERVER_PEERS][32];
    uint8_t mac1key_server[32];
    uint8_t mac1key_client[32];
    uint8_t server_peer_mac1keys[AWG_MAX_SERVER_PEERS][32];
    int server_peer_count;

    uint32_t h4_fixed;
    int h4_noop;        /* H4={4,4} && S4==0 */
    int init_total;     /* S1 + 148 */
    int resp_total;     /* S2 + 92 */
    int cookie_total;   /* S3 + 64 */
    int has_server_pub; /* server_pub != zero */
    int has_client_pub; /* client_pub != zero */
    const uint8_t *mac1key_out; /* MAC1 key for outbound (WG→AWG) recompute */
    const uint8_t *mac1key_in;  /* MAC1 key for inbound (AWG→WG) recompute */
    int transport_size_ambiguous; /* handshake size can overlap transport */

    int timeout;        /* seconds, default 180 */
    int dns_refresh;    /* periodic DNS re-resolve interval, seconds, 0 = off */
    int log_level;
    int socket_buf;     /* socket buffer size */
    int src_port;       /* 0 = auto */

    int cpu_c2s;        /* CPU affinity for c2s thread (-1 = auto) */
    int cpu_s2c;        /* CPU affinity for s2c thread (-1 = auto) */
    int busy_poll;      /* SO_BUSY_POLL timeout in μs (0 = off) */
    int no_gro;         /* disable UDP GRO (AWG_NO_GRO=1) */
    int no_df;          /* clear DF bit on UDP sockets (AWG_NO_DF=1) */

    int mode;           /* 0=normal, 1=reverse, 2=server */

    /* Dual-profile fallback (site-to-site backward compat; normal/reverse only).
     * The active profile's fields are mirrored into the flat fields above; the
     * hot path always reads the flat fields. Switching happens only while the
     * tunnel is down or during a handshake, never mid-stream. */
    awg_profile_t profiles[2]; /* [0]=primary, [1]=v1 fallback */
    int profile_count;         /* 1 = no fallback, 2 = fallback enabled */
    int active_profile;        /* index currently mirrored into the flat fields */
    int fb_after;              /* initiator: seconds of remote silence before probing the other profile */
} awg_config_t;

#define AWG_MODE_NORMAL  0
#define AWG_MODE_REVERSE 1
#define AWG_MODE_SERVER  2

/* Compute MAC1 keys and fast-path flags. Call after setting all config fields. */
void config_compute(awg_config_t *cfg);

/* Compute the derived fast-path flags of a single profile from its base fields. */
void config_compute_profile(awg_profile_t *pr);

/* Snapshot the active (flat) profile fields of cfg into pr. */
void config_snapshot_profile(const awg_config_t *cfg, awg_profile_t *pr);

/* Mirror profiles[idx] into the flat fields and set active_profile = idx.
 * Shared fields (keys, mac1) are untouched. Refill the H4 ring after this. */
void config_apply_profile(awg_config_t *cfg, int idx);

/* Validate config values that participate in buffer sizing and layout. */
int config_validate(const awg_config_t *cfg, const char **err_msg);

/* Same validation for a standalone profile (used for the fallback profile). */
int config_validate_profile(const awg_profile_t *pr, const char **err_msg);

/* Transform outbound WG->AWG. Returns output pointer and length.
 * buf has dataoff bytes of headroom before the packet data.
 * sendJunk is set to 1 if junk should be sent before this packet. */
uint8_t *transform_outbound(uint8_t *buf, int dataoff, int n,
                             const awg_config_t *cfg, uint64_t rand_val,
                             int *out_len, int *sendJunk);

/* Same as transform_outbound(), but allows overriding the outbound MAC1 key
 * for handshake packets. Pass NULL to use cfg->mac1key_out. */
uint8_t *transform_outbound_with_mac1(uint8_t *buf, int dataoff, int n,
                                      const awg_config_t *cfg,
                                      const uint8_t *mac1key_out,
                                      uint64_t rand_val,
                                      int *out_len, int *sendJunk);

/* Transform inbound AWG->WG. Returns output pointer and length, or NULL if invalid/junk. */
uint8_t *transform_inbound(uint8_t *buf, int n, const awg_config_t *cfg, int *out_len);

/* In server mode, match an original WireGuard handshake response against the
 * configured explicit client peer list. Returns peer index or -1 if no match. */
int config_server_resolve_peer_for_response(const awg_config_t *cfg,
                                            const uint8_t *wg_resp, int n);

/* Generate junk packets into pre-allocated buffer.
 * junk_buf: buffer of at least jc*jmax bytes (pre-filled with random).
 * sizes[]: output array of packet sizes (at least jc entries).
 * Returns number of junk packets. */
int generate_junk(const awg_config_t *cfg, uint8_t *junk_buf, int *sizes);

#endif
