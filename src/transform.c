#include "transform.h"
#include "blake2s.h"
#include "chacha20.h"
#include "fastrand.h"
#include "csprng.h"
#include <string.h>

/* Static buffer for handshake packets when headroom is insufficient.
 * Handshakes are rare (1-2 per connection), so static is fine. */
static __thread uint8_t hs_buf[AWG_PACKET_BUF_SIZE];

int transform_is_shared_buf(const uint8_t *p) {
    return p >= hs_buf && p < hs_buf + sizeof(hs_buf);
}

static int hrange_overlaps(const hrange_t *a, const hrange_t *b) {
    return a->min <= b->max && b->min <= a->max;
}

int config_validate_profile(const awg_profile_t *pr, const char **err_msg) {
    if (pr->s1 < 0 || pr->s1 > AWG_PACKET_BUF_SIZE - WG_INIT_SIZE) {
        *err_msg = "AWG_S1: must be between 0 and 1352";
        return -1;
    }
    if (pr->s2 < 0 || pr->s2 > AWG_PACKET_BUF_SIZE - WG_RESP_SIZE) {
        *err_msg = "AWG_S2: must be between 0 and 1408";
        return -1;
    }
    if (pr->s3 < 0 || pr->s3 > AWG_PACKET_BUF_SIZE - WG_COOKIE_SIZE) {
        *err_msg = "AWG_S3: must be between 0 and 1436";
        return -1;
    }
    if (pr->s4 < 0 || pr->s4 > AWG_PACKET_HEADROOM) {
        *err_msg = "AWG_S4: must be between 0 and 256";
        return -1;
    }
    /* Header protection derives its nonce from the first 12 padding bytes, so
     * every padding must be at least that long (same rule as amneziawg-go). */
    if (pr->hp_on) {
        if (pr->s1 < AWG_HP_MIN_PADDING) {
            *err_msg = "AWG_S1: must be >= 12 when AWG_HEADER_PROTECTION_KEY is set";
            return -1;
        }
        if (pr->s2 < AWG_HP_MIN_PADDING) {
            *err_msg = "AWG_S2: must be >= 12 when AWG_HEADER_PROTECTION_KEY is set";
            return -1;
        }
        if (pr->s3 < AWG_HP_MIN_PADDING) {
            *err_msg = "AWG_S3: must be >= 12 when AWG_HEADER_PROTECTION_KEY is set";
            return -1;
        }
        if (pr->s4 < AWG_HP_MIN_PADDING) {
            *err_msg = "AWG_S4: must be >= 12 when AWG_HEADER_PROTECTION_KEY is set";
            return -1;
        }
    }
    if (hrange_overlaps(&pr->h1, &pr->h2) ||
        hrange_overlaps(&pr->h1, &pr->h3) ||
        hrange_overlaps(&pr->h1, &pr->h4) ||
        hrange_overlaps(&pr->h2, &pr->h3) ||
        hrange_overlaps(&pr->h2, &pr->h4) ||
        hrange_overlaps(&pr->h3, &pr->h4)) {
        *err_msg = "AWG_H1..AWG_H4: ranges must not overlap";
        return -1;
    }

    *err_msg = NULL;
    return 0;
}

int config_validate(const awg_config_t *cfg, const char **err_msg) {
    awg_profile_t pr = {
        .s1 = cfg->s1, .s2 = cfg->s2, .s3 = cfg->s3, .s4 = cfg->s4,
        .h1 = cfg->h1, .h2 = cfg->h2, .h3 = cfg->h3, .h4 = cfg->h4,
        .hp_on = cfg->hp_on,
    };
    /* The junk buffer is sized jc * jmax, so an inverted range is a buffer
     * overrun waiting to happen rather than a harmless typo. */
    if (cfg->jc > 0 && cfg->jmin > cfg->jmax) {
        *err_msg = "AWG_JMIN: must not be greater than AWG_JMAX";
        return -1;
    }
    return config_validate_profile(&pr, err_msg);
}

void config_compute_profile(awg_profile_t *pr) {
    pr->h4_fixed = pr->h4.min;
    pr->h4_noop = (pr->h4.min == WG_TRANSPORT_DATA &&
                   pr->h4.max == WG_TRANSPORT_DATA && pr->s4 == 0 && !pr->hp_on);
    pr->init_total = pr->s1 + WG_INIT_SIZE;
    pr->resp_total = pr->s2 + WG_RESP_SIZE;
    pr->cookie_total = pr->s3 + WG_COOKIE_SIZE;

    int tmin = pr->s4 + WG_TRANSPORT_MIN;
    pr->transport_size_ambiguous =
        (pr->init_total >= tmin) ||
        (pr->resp_total >= tmin) ||
        (pr->cookie_total >= tmin);
}

void config_snapshot_profile(const awg_config_t *cfg, awg_profile_t *pr) {
    pr->s1 = cfg->s1; pr->s2 = cfg->s2; pr->s3 = cfg->s3; pr->s4 = cfg->s4;
    pr->h1 = cfg->h1; pr->h2 = cfg->h2; pr->h3 = cfg->h3; pr->h4 = cfg->h4;
    for (int i = 0; i < 5; i++) pr->cps[i] = cfg->cps[i];
    pr->hp_on = cfg->hp_on;
    pr->rt = cfg->rt;
    pr->h4_fixed = cfg->h4_fixed;
    pr->h4_noop = cfg->h4_noop;
    pr->init_total = cfg->init_total;
    pr->resp_total = cfg->resp_total;
    pr->cookie_total = cfg->cookie_total;
    pr->transport_size_ambiguous = cfg->transport_size_ambiguous;
}

void config_apply_profile(awg_config_t *cfg, int idx) {
    const awg_profile_t *pr = &cfg->profiles[idx];
    cfg->s1 = pr->s1; cfg->s2 = pr->s2; cfg->s3 = pr->s3; cfg->s4 = pr->s4;
    cfg->h1 = pr->h1; cfg->h2 = pr->h2; cfg->h3 = pr->h3; cfg->h4 = pr->h4;
    for (int i = 0; i < 5; i++) cfg->cps[i] = pr->cps[i];
    cfg->hp_on = pr->hp_on;
    cfg->rt = pr->rt;
    cfg->h4_fixed = pr->h4_fixed;
    cfg->h4_noop = pr->h4_noop;
    cfg->init_total = pr->init_total;
    cfg->resp_total = pr->resp_total;
    cfg->cookie_total = pr->cookie_total;
    cfg->transport_size_ambiguous = pr->transport_size_ambiguous;
    cfg->active_profile = idx;
}

void config_compute_max_s4(awg_config_t *cfg) {
    int m = 0, rt = 0;
    int n = cfg->profile_count > 0 ? cfg->profile_count : 1;
    for (int i = 0; i < n; i++) {
        if (cfg->profiles[i].s4 > m) m = cfg->profiles[i].s4;
        rt |= cfg->profiles[i].rt;
    }
    cfg->max_s4 = m;
    /* Server mode hands each client its own profile, so the window has to be
     * kept up to date whenever *any* stage of the chain sends trailers. */
    cfg->rt_any = rt;
}

void config_compute(awg_config_t *cfg) {
    static const uint8_t z[32] = {0};
    cfg->has_server_pub = memcmp(cfg->server_pub, z, 32) != 0;
    cfg->has_client_pub = memcmp(cfg->client_pub, z, 32) != 0;
    cfg->hp_key_set = memcmp(cfg->hp_key, z, 32) != 0;

    compute_mac1_key(cfg->server_pub, cfg->mac1key_server);
    compute_mac1_key(cfg->client_pub, cfg->mac1key_client);
    for (int i = 0; i < cfg->server_peer_count; i++)
        compute_mac1_key(cfg->server_peer_pubs[i], cfg->server_peer_mac1keys[i]);

    awg_profile_t pr = {
        .s1 = cfg->s1, .s2 = cfg->s2, .s3 = cfg->s3, .s4 = cfg->s4,
        .h1 = cfg->h1, .h2 = cfg->h2, .h3 = cfg->h3, .h4 = cfg->h4,
        .hp_on = cfg->hp_on,
    };
    config_compute_profile(&pr);
    cfg->h4_fixed = pr.h4_fixed;
    cfg->h4_noop = pr.h4_noop;
    cfg->init_total = pr.init_total;
    cfg->resp_total = pr.resp_total;
    cfg->cookie_total = pr.cookie_total;
    cfg->transport_size_ambiguous = pr.transport_size_ambiguous;

    if (cfg->mode == AWG_MODE_NORMAL) {
        cfg->mac1key_out = cfg->has_server_pub ? cfg->mac1key_server : NULL;
        cfg->mac1key_in  = cfg->has_client_pub ? cfg->mac1key_client : NULL;
    } else {
        cfg->mac1key_out = cfg->has_client_pub ? cfg->mac1key_client : NULL;
        cfg->mac1key_in  = cfg->has_server_pub ? cfg->mac1key_server : NULL;
    }

    /* The window only ever grows from here, so a fresh config starts at the
     * same floor amneziawg uses for a peer that has not sent anything yet. */
    if (atomic_load_explicit(&cfg->udp_window, memory_order_relaxed) < AWG_DEFAULT_UDP_WINDOW)
        atomic_store_explicit(&cfg->udp_window, AWG_DEFAULT_UDP_WINDOW, memory_order_relaxed);

    /* profiles[0] always mirrors the flat fields: the active profile is what
     * transform_inbound/outbound resolve to. */
    if (cfg->profile_count < 1) cfg->profile_count = 1;
    cfg->active_profile = 0;
    config_snapshot_profile(cfg, &cfg->profiles[0]);
    config_compute_max_s4(cfg);
}

static inline uint32_t read32_le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void write32_le(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

/* Fill the S padding that precedes a message.
 *
 * The padding goes on the wire raw, so it has to be indistinguishable from
 * random to an observer — that is its entire job. xorshift64 is not: its output
 * is its state, so eight observed bytes give the next eight exactly, and a
 * censor confirms "this is awg-proxy" from a single packet. Under v3 the first
 * 12 bytes are also the ChaCha20 nonce. amneziawg-go fills the same field with
 * crypto/rand (device/send.go), and so do we. */
static inline void fill_padding(uint8_t *dst, int len) {
    csprng_bytes(dst, (size_t)len);
}

/* A cheap mixer for values the observer sees only as a consequence, not as
 * bytes: it turns a live fastrand state word into a number that no longer
 * equals the caller's next output. Padding no longer goes through it — that
 * is csprng_bytes() now — but the AWG 3.1 trailer length still does. */
static inline uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/* AWG 3.1: length of the random trailer for an outbound handshake, mirroring
 * amneziawg's peer.randomTrailer() — a draw from [0, window - size). The window
 * is capped at the packet buffer, so padding + message + trailer always fits
 * both the caller's buffer and hs_buf. */
static inline int trailer_len(const awg_config_t *cfg, const awg_profile_t *pr,
                              int size, uint64_t seed) {
    if (!pr->rt) return 0;
    uint32_t win = atomic_load_explicit(&cfg->udp_window, memory_order_relaxed);
    if (win > AWG_PACKET_BUF_SIZE) win = AWG_PACKET_BUF_SIZE;
    if ((int)win <= size) return 0;
    return (int)(mix64(seed ^ 0x7A11E45ull) % (uint32_t)((int)win - size));
}

/* Append that trailer to a finished packet and return its new length. The
 * trailer stays in the clear under header protection: only the message itself
 * is encrypted, exactly as in send.go. */
static inline int append_trailer(const awg_config_t *cfg, const awg_profile_t *pr,
                                 uint8_t *pkt, int size, uint64_t seed) {
    int tl = trailer_len(cfg, pr, size, seed);
    if (tl > 0)
        fill_padding(pkt + size, tl);
    return size + tl;
}

__attribute__((hot))
uint8_t *transform_outbound_profile(uint8_t *buf, int dataoff, int n,
                                    const awg_config_t *cfg,
                                    const awg_profile_t *pr,
                                    const uint8_t *mac1key_out,
                                    uint64_t rand_val,
                                    int *out_len, int *sendJunk) {
    const uint8_t *out_key = mac1key_out ? mac1key_out : cfg->mac1key_out;
    const int hp = pr->hp_on;

    *sendJunk = 0;
    if (n < 4) {
        *out_len = n;
        return buf + dataoff;
    }

    uint8_t *data = buf + dataoff;
    uint32_t msgType = read32_le(data);

    /* Handshake messages: write the AWG type, recompute MAC1 over the rewritten
     * message, then (v3) encrypt the whole message. Order matches send.go. */
    if (msgType == WG_HANDSHAKE_INIT && n == WG_INIT_SIZE) {
        write32_le(data, hrange_pick(&pr->h1, rand_val));
        if (out_key)
            recompute_mac1(data, out_key);
        *sendJunk = (cfg->jc > 0);
        if (pr->s1 > 0) {
            uint8_t *out;
            if (dataoff >= pr->s1) {
                out = data - pr->s1;
            } else {
                /* Headroom insufficient: use static buffer (handshakes are rare) */
                memcpy(hs_buf + pr->s1, data, (size_t)n);
                out = hs_buf;
            }
            fill_padding(out, pr->s1);
            if (hp) chacha20_xor(cfg->hp_key, out, out + pr->s1, n);
            *out_len = append_trailer(cfg, pr, out, pr->s1 + n, rand_val);
            return out;
        }
        *out_len = append_trailer(cfg, pr, data, n, rand_val);
        return data;
    }

    if (msgType == WG_HANDSHAKE_RESPONSE && n == WG_RESP_SIZE) {
        write32_le(data, hrange_pick(&pr->h2, rand_val));
        if (out_key)
            recompute_mac1_response(data, out_key);
        if (pr->s2 > 0) {
            uint8_t *out;
            if (dataoff >= pr->s2) {
                out = data - pr->s2;
            } else {
                memcpy(hs_buf + pr->s2, data, (size_t)n);
                out = hs_buf;
            }
            fill_padding(out, pr->s2);
            if (hp) chacha20_xor(cfg->hp_key, out, out + pr->s2, n);
            *out_len = append_trailer(cfg, pr, out, pr->s2 + n, rand_val ^ 0x12345);
            return out;
        }
        *out_len = append_trailer(cfg, pr, data, n, rand_val ^ 0x12345);
        return data;
    }

    if (msgType == WG_COOKIE_REPLY && n == WG_COOKIE_SIZE) {
        /* v3.1 DisableCookies: the interface never answers with a cookie, so
         * the reply the local WireGuard produced is dropped here. */
        if (cfg->disable_cookies) {
            *out_len = 0;
            return NULL;
        }
        write32_le(data, hrange_pick(&pr->h3, rand_val));
        if (pr->s3 > 0) {
            uint8_t *out;
            if (dataoff >= pr->s3) {
                out = data - pr->s3;
            } else {
                memcpy(hs_buf + pr->s3, data, (size_t)n);
                out = hs_buf;
            }
            fill_padding(out, pr->s3);
            if (hp) chacha20_xor(cfg->hp_key, out, out + pr->s3, n);
            *out_len = append_trailer(cfg, pr, out, pr->s3 + n, rand_val ^ 0x67890);
            return out;
        }
        *out_len = append_trailer(cfg, pr, data, n, rand_val ^ 0x67890);
        return data;
    }

    if (msgType == WG_TRANSPORT_DATA && n >= WG_TRANSPORT_MIN) {
        if (pr->h4_noop) {
            *out_len = n;
            return data;
        }
        if (pr->h4.min == pr->h4.max)
            write32_le(data, pr->h4_fixed);
        else
            write32_le(data, hrange_pick(&pr->h4, rand_val));
        if (pr->s4 > 0 && dataoff >= pr->s4) {
            uint8_t *out = data - pr->s4;
            fill_padding(out, pr->s4);
            if (hp) chacha20_xor(cfg->hp_key, out, data, AWG_HP_TRANSPORT_HDR);
            *out_len = pr->s4 + n;
            return out;
        }
        *out_len = n;
        return data;
    }

    /* Unknown, pass through */
    *out_len = n;
    return data;
}

__attribute__((hot))
uint8_t *transform_outbound_with_mac1(uint8_t *buf, int dataoff, int n,
                                      const awg_config_t *cfg,
                                      const uint8_t *mac1key_out,
                                      uint64_t rand_val,
                                      int *out_len, int *sendJunk) {
    return transform_outbound_profile(buf, dataoff, n, cfg,
                                      config_active_profile(cfg), mac1key_out,
                                      rand_val, out_len, sendJunk);
}

__attribute__((hot))
uint8_t *transform_outbound(uint8_t *buf, int dataoff, int n,
                             const awg_config_t *cfg, uint64_t rand_val,
                             int *out_len, int *sendJunk) {
    return transform_outbound_profile(buf, dataoff, n, cfg,
                                      config_active_profile(cfg), NULL,
                                      rand_val, out_len, sendJunk);
}

int config_server_resolve_peer_for_response(const awg_config_t *cfg,
                                            const uint8_t *wg_resp, int n) {
    uint8_t mac1[16];

    if (!cfg || !wg_resp || n != WG_RESP_SIZE || read32_le(wg_resp) != WG_HANDSHAKE_RESPONSE)
        return -1;

    for (int i = 0; i < cfg->server_peer_count; i++) {
        blake2s_128mac(cfg->server_peer_mac1keys[i], wg_resp, 60, mac1);
        if (memcmp(mac1, wg_resp + 60, 16) == 0)
            return i;
    }
    return -1;
}

/* Same trick for a handshake the server starts on its own. Such an init has no
 * receiver_index, so there is nothing to look up in the session table — but its
 * MAC1 is keyed on the *recipient's* static key, which here is the client's,
 * and the hub holds every client key. Trying them in turn names the target.
 * Without this the only fallback is "route it to the sole client", which stops
 * working the moment a second client exists. */
int config_server_resolve_peer_for_init(const awg_config_t *cfg,
                                        const uint8_t *wg_init, int n) {
    uint8_t mac1[16];

    if (!cfg || !wg_init || n != WG_INIT_SIZE || read32_le(wg_init) != WG_HANDSHAKE_INIT)
        return -1;

    for (int i = 0; i < cfg->server_peer_count; i++) {
        blake2s_128mac(cfg->server_peer_mac1keys[i], wg_init, 116, mac1);
        if (memcmp(mac1, wg_init + 116, 16) == 0)
            return i;
    }
    return -1;
}

__attribute__((hot))
uint8_t *transform_inbound_profile(uint8_t *buf, int n, const awg_config_t *cfg,
                                   const awg_profile_t *pr, awg_hp_ks_t *ks,
                                   int *out_len) {
    if (n < 4) return NULL;

    /* Fast path: identity transform (never taken when hp is on — h4_noop
     * requires S4 == 0, which header protection forbids) */
    if (pr->h4_noop) {
        if (read32_le(buf) == WG_TRANSPORT_DATA && n >= WG_TRANSPORT_MIN) {
            *out_len = n;
            return buf;
        }
    }

    /* v3: the type is XOR'd with the first 4 keystream bytes. One block covers
     * every candidate padding — the nonce is the datagram's first 12 bytes. */
    awg_hp_ks_t local;
    const uint8_t *kstream = NULL;
    uint32_t type_mask = 0;
    if (pr->hp_on) {
        if (n < AWG_HP_MIN_PADDING) return NULL;
        if (!ks) { local.valid = 0; ks = &local; }
        kstream = hp_recv_ks(cfg, buf, ks);
        type_mask = read32_le(kstream);
    }

    /* Size-based dispatch: handshake first, transport last. With v3.1 random
     * trailers the peer appends an arbitrary tail, so any length at or above
     * the expected one qualifies and the tail is cut off by returning the fixed
     * message size — same rule as receive.go's pskb_trim. */
    if (n == pr->init_total || (pr->rt && n > pr->init_total)) {
        uint32_t h = read32_le(buf + pr->s1) ^ type_mask;
        if (hrange_contains(&pr->h1, h)) {
            if (kstream) chacha20_xor(cfg->hp_key, buf, buf + pr->s1, WG_INIT_SIZE);
            write32_le(buf + pr->s1, WG_HANDSHAKE_INIT);
            if (cfg->mac1key_in)
                recompute_mac1(buf + pr->s1, cfg->mac1key_in);
            *out_len = WG_INIT_SIZE;
            return buf + pr->s1;
        }
    }

    if (n == pr->resp_total || (pr->rt && n > pr->resp_total)) {
        uint32_t h = read32_le(buf + pr->s2) ^ type_mask;
        if (hrange_contains(&pr->h2, h)) {
            if (kstream) chacha20_xor(cfg->hp_key, buf, buf + pr->s2, WG_RESP_SIZE);
            write32_le(buf + pr->s2, WG_HANDSHAKE_RESPONSE);
            if (cfg->mac1key_in)
                recompute_mac1_response(buf + pr->s2, cfg->mac1key_in);
            *out_len = WG_RESP_SIZE;
            return buf + pr->s2;
        }
    }

    if (n == pr->cookie_total || (pr->rt && n > pr->cookie_total)) {
        uint32_t h = read32_le(buf + pr->s3) ^ type_mask;
        if (hrange_contains(&pr->h3, h)) {
            if (kstream) chacha20_xor(cfg->hp_key, buf, buf + pr->s3, WG_COOKIE_SIZE);
            write32_le(buf + pr->s3, WG_COOKIE_REPLY);
            *out_len = WG_COOKIE_SIZE;
            return buf + pr->s3;
        }
    }

    /* Transport data: variable size, checked last */
    if (n >= pr->s4 + WG_TRANSPORT_MIN) {
        uint32_t h = read32_le(buf + pr->s4) ^ type_mask;
        if (hrange_contains(&pr->h4, h)) {
            if (kstream)
                chacha20_xor_ks(buf + pr->s4, kstream, AWG_HP_TRANSPORT_HDR);
            write32_le(buf + pr->s4, WG_TRANSPORT_DATA);
            *out_len = n - pr->s4;
            return buf + pr->s4;
        }
    }

    return NULL;
}

__attribute__((hot))
uint8_t *transform_inbound(uint8_t *buf, int n, const awg_config_t *cfg, int *out_len) {
    return transform_inbound_profile(buf, n, cfg, config_active_profile(cfg),
                                     NULL, out_len);
}

int generate_junk(const awg_config_t *cfg, uint8_t *junk_buf, int *sizes) {
    if (cfg->jc <= 0 || cfg->jmax <= 0) return 0;

    /* Clamp downward, never upward: the caller sized junk_buf as jc * cfg->jmax
     * (proxy.c), so raising jmax to meet a larger jmin would send sizes the
     * allocation cannot cover and sendto() would read adjacent heap onto the
     * wire. config_validate rejects jmin > jmax outright; this keeps the buffer
     * safe even if some future caller skips validation. */
    int jmax = cfg->jmax;
    int jmin = cfg->jmin > 0 ? cfg->jmin : 1;
    if (jmin > jmax) jmin = jmax;
    /* Half-open [jmin, jmax), matching amneziawg-go's min + fastrandn(max-min) */
    int span = jmax - jmin;

    /* junk_buf should already be filled with random data by caller */
    fastrand_t r;
    fastrand_init(&r, read32_le(junk_buf) | 1);

    for (int i = 0; i < cfg->jc; i++) {
        sizes[i] = (span > 0) ? jmin + fastrand_intn(&r, span) : jmin;
    }
    return cfg->jc;
}
