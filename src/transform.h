#ifndef AWG_TRANSFORM_H
#define AWG_TRANSFORM_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
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

/* Shared fixed buffer sizes used by the proxy data paths */
#define AWG_PACKET_BUF_SIZE 1500
#define AWG_PACKET_HEADROOM 256

/* --- AWG 3.0 header protection ---
 * The only wire-format change in 3.0: the message header is ChaCha20-encrypted
 * with the first 12 bytes of the S padding as nonce. Transport packets encrypt
 * only Type(4)|Receiver(4)|Counter(8); handshakes encrypt the whole message.
 * The padding itself stays in the clear, so every S must be at least 12. */
#define AWG_HP_MIN_PADDING   CHACHA20_NONCE_SIZE
#define AWG_HP_TRANSPORT_HDR 16

/* --- AWG 3.1 random trailers ---
 * A random number of bytes is appended to every handshake, response and cookie
 * packet, so their sizes stop being a fingerprint. The receiver accepts any
 * length at or above the expected one and cuts the tail off. Transport packets
 * carry their trailer *inside* the AEAD as ordinary content padding, which the
 * peer's WireGuard strips on its own — the proxy holds no keys and therefore
 * neither adds nor removes anything there.
 *
 * The trailer length is drawn from [0, udp_window - packet_size): the window is
 * the largest datagram seen on this connection (never below 500, never above
 * the packet buffer), so a padded handshake never stands out against the
 * transport traffic around it. */
#define AWG_DEFAULT_UDP_WINDOW 500

/* Fallback chain length: v3 -> v2 -> v1.5 -> v1 */
#define AWG_MAX_PROFILES 4

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
#define CPS_ZEROS        'Z'   /* <dz N>: N bytes holding the big-endian source
                                * length, which is always 0 for an I-packet */

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
 * versions. The proxy keeps up to AWG_MAX_PROFILES (primary + fallback chain)
 * and can switch the active one at runtime for backward compatibility. Buffer
 * headroom is sized from the largest S4 across profiles, so switching never
 * overflows the packet layout. */
typedef struct {
    int s1, s2, s3, s4;
    hrange_t h1, h2, h3, h4;
    cps_template_t *cps[5]; /* I1-I5, NULL if not configured */
    int hp_on;              /* v3: this profile encrypts the header */
    int rt;                 /* v3.1: append a random trailer to handshakes */
    /* Derived (filled by config_compute_profile) */
    uint32_t h4_fixed;
    int h4_noop;
    int init_total;
    int resp_total;
    int cookie_total;
    int transport_size_ambiguous;
} awg_profile_t;

/* First ChaCha20 keystream block of a received packet. The nonce is the
 * packet's first 12 bytes, so the block is the same for every profile — it is
 * computed at most once and reused across fallback-chain attempts. */
typedef struct {
    uint8_t ks[CHACHA20_BLOCK_SIZE];
    int valid;
} awg_hp_ks_t;

/* Config struct */
#define AWG_MAX_SERVER_PEERS 256

/* Opening bid for the spin-drain controller: the rung it starts on. */
#define SPIN_START_US 200

typedef struct {
    int jc, jmin, jmax;
    int s1, s2, s3, s4;
    hrange_t h1, h2, h3, h4;

    cps_template_t *cps[5]; /* I1-I5, NULL if not configured */

    uint8_t server_pub[32];
    uint8_t client_pub[32];
    uint8_t server_peer_pubs[AWG_MAX_SERVER_PEERS][32];
    uint8_t mac1key_server[32];
    uint8_t mac1key_client[32];
    uint8_t server_peer_mac1keys[AWG_MAX_SERVER_PEERS][32];
    int server_peer_count;

    /* AWG 3.0 header protection key — one per device, shared by all profiles.
     * Zero key means "no v3 anywhere", exactly as in amneziawg-go. */
    uint8_t hp_key[32];
    int hp_key_set;

    uint32_t h4_fixed;
    int h4_noop;        /* H4={4,4} && S4==0 */
    int init_total;     /* S1 + 148 */
    int resp_total;     /* S2 + 92 */
    int cookie_total;   /* S3 + 64 */
    int hp_on;          /* active profile encrypts the header */
    int rt;             /* active profile appends random trailers */
    int rt_any;         /* any profile does — gates the window bookkeeping */
    int disable_cookies; /* v3.1: never forward a cookie reply outbound */
    /* Largest datagram seen on this connection — the ceiling for trailer
     * lengths. Written by both I/O threads, read by the transform: a stale or
     * torn value would only pick a different trailer size, so relaxed atomics
     * are enough and no lock is taken on the hot path. */
    _Atomic uint32_t udp_window;
    int has_server_pub; /* server_pub != zero */
    int has_client_pub; /* client_pub != zero */
    const uint8_t *mac1key_out; /* MAC1 key for outbound (WG→AWG) recompute */
    const uint8_t *mac1key_in;  /* MAC1 key for inbound (AWG→WG) recompute */
    int transport_size_ambiguous; /* handshake size can overlap transport */

    int timeout;        /* seconds, default 180 */
    int dns_refresh;    /* periodic DNS re-resolve interval, seconds, 0 = off */
    int he_delay;       /* Happy Eyeballs: ms of IPv4 silence before probing IPv6 */
    const char *state_file; /* learned-preference file, NULL/"" = don't persist */
    int log_level;
    int socket_buf;     /* socket buffer size */
    int src_port;       /* 0 = auto */

    int cpu_c2s;        /* CPU affinity for c2s thread (-1 = auto) */
    int cpu_s2c;        /* CPU affinity for s2c thread (-1 = auto) */
    int busy_poll;      /* SO_BUSY_POLL timeout in μs (0 = off) */
    int spin_us;        /* userspace spin-drain budget in μs (0 = off) */
    int spin_auto;      /* AWG_SPIN=auto: tune that budget at runtime */
    int no_gro;         /* disable UDP GRO (AWG_NO_GRO=1) */
    int no_df;          /* clear DF bit on UDP sockets (AWG_NO_DF=1) */
    int stats_interval; /* seconds between throughput/drop stat lines, 0 = off */

    int mode;           /* 0=normal, 1=reverse, 2=server */

    /* Profile fallback chain (v3 → v2 → v1.5 → v1). The active profile's
     * fields are mirrored into the flat fields above; normal/reverse hot paths
     * read the flat fields. Server mode keeps a per-client profile instead, so
     * clients on different versions never override each other. */
    awg_profile_t profiles[AWG_MAX_PROFILES]; /* [0]=primary, [1..]=fallbacks */
    int profile_count;         /* 1 = no fallback */
    int active_profile;        /* index currently mirrored into the flat fields */
    int max_s4;                /* max S4 across profiles — sizes buffer headroom */
    int fb_after;              /* initiator: seconds of remote silence before probing the next profile */
} awg_config_t;

#define AWG_MODE_NORMAL  0
#define AWG_MODE_REVERSE 1
#define AWG_MODE_SERVER  2

/* Note a datagram size against the trailer window (no-op unless trailers are
 * on). Called from the transport fast paths, where cfg is never const. */
static inline void awg_window_note(awg_config_t *cfg, int size) {
    if (!cfg->rt_any) return;
    if ((uint32_t)size > atomic_load_explicit(&cfg->udp_window, memory_order_relaxed))
        atomic_store_explicit(&cfg->udp_window, (uint32_t)size, memory_order_relaxed);
}

/* Compute MAC1 keys and fast-path flags. Call after setting all config fields. */
void config_compute(awg_config_t *cfg);

/* Compute the derived fast-path flags of a single profile from its base fields. */
void config_compute_profile(awg_profile_t *pr);

/* Snapshot the active (flat) profile fields of cfg into pr. */
void config_snapshot_profile(const awg_config_t *cfg, awg_profile_t *pr);

/* Mirror profiles[idx] into the flat fields and set active_profile = idx.
 * Shared fields (keys, mac1) are untouched. Refill the H4 ring after this. */
void config_apply_profile(awg_config_t *cfg, int idx);

/* Recompute the per-chain derived fields (max_s4, rt_any) across profiles.
 * Call after the fallback stages have been parsed. */
void config_compute_max_s4(awg_config_t *cfg);

/* Validate config values that participate in buffer sizing and layout. */
int config_validate(const awg_config_t *cfg, const char **err_msg);

/* Same validation for a standalone profile (used for fallback profiles). */
int config_validate_profile(const awg_profile_t *pr, const char **err_msg);

static inline const awg_profile_t *config_active_profile(const awg_config_t *cfg) {
    return &cfg->profiles[cfg->active_profile];
}

/* Transform outbound WG->AWG using an explicit profile.
 * buf has dataoff bytes of headroom before the packet data; the S padding is
 * written into the last pr->s4 (or s1/s2/s3) bytes of it.
 * mac1key_out overrides cfg->mac1key_out for handshakes (NULL = use config).
 * sendJunk is set to 1 if junk should be sent before this packet. */
uint8_t *transform_outbound_profile(uint8_t *buf, int dataoff, int n,
                                    const awg_config_t *cfg,
                                    const awg_profile_t *pr,
                                    const uint8_t *mac1key_out,
                                    uint64_t rand_val,
                                    int *out_len, int *sendJunk);

/* True if p points into the thread-local buffer transform_outbound* falls back
 * to when the caller's headroom is smaller than the padding. That buffer is
 * shared by every packet of the thread, so a caller that queues packets (a
 * sendmmsg batch) must send such a packet before transforming the next one. */
int transform_is_shared_buf(const uint8_t *p);

/* Same, on the active profile. */
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

/* Transform inbound AWG->WG using an explicit profile. Returns output pointer
 * and length, or NULL if invalid/junk. ks may be NULL; when supplied with
 * ks->valid == 0 it caches the header-protection keystream block so repeated
 * attempts over the profile chain compute ChaCha20 only once. */
uint8_t *transform_inbound_profile(uint8_t *buf, int n, const awg_config_t *cfg,
                                   const awg_profile_t *pr, awg_hp_ks_t *ks,
                                   int *out_len);

/* Same, on the active profile. */
uint8_t *transform_inbound(uint8_t *buf, int n, const awg_config_t *cfg, int *out_len);

/* Header-protection keystream block for a received packet (nonce = first 12
 * bytes). Cached in ks. */
static inline const uint8_t *hp_recv_ks(const awg_config_t *cfg,
                                        const uint8_t *pkt, awg_hp_ks_t *ks) {
    if (!ks->valid) {
        chacha20_block(cfg->hp_key, pkt, 0, ks->ks);
        ks->valid = 1;
    }
    return ks->ks;
}

/* In server mode, match an original WireGuard handshake response against the
 * configured explicit client peer list. Returns peer index or -1 if no match. */
int config_server_resolve_peer_for_response(const awg_config_t *cfg,
                                            const uint8_t *wg_resp, int n);

/* Same for a server-initiated handshake init: its MAC1 is keyed on the
 * client's static key, so the peer list identifies the target. Returns peer
 * index or -1. */
int config_server_resolve_peer_for_init(const awg_config_t *cfg,
                                        const uint8_t *wg_init, int n);

/* Generate junk packets into pre-allocated buffer.
 * junk_buf: buffer of at least jc*jmax bytes (pre-filled with random).
 * sizes[]: output array of packet sizes (at least jc entries).
 * Returns number of junk packets. */
int generate_junk(const awg_config_t *cfg, uint8_t *junk_buf, int *sizes);

#endif
