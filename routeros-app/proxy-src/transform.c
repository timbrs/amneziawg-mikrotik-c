#include "transform.h"
#include "blake2s.h"
#include "fastrand.h"
#include <string.h>

/* Static buffer for handshake packets when headroom is insufficient.
 * Handshakes are rare (1-2 per connection), so static is fine. */
static __thread uint8_t hs_buf[AWG_PACKET_BUF_SIZE];

static int hrange_overlaps(const hrange_t *a, const hrange_t *b) {
    return a->min <= b->max && b->min <= a->max;
}

int config_validate(const awg_config_t *cfg, const char **err_msg) {
    if (cfg->s1 < 0 || cfg->s1 > AWG_PACKET_BUF_SIZE - WG_INIT_SIZE) {
        *err_msg = "AWG_S1: must be between 0 and 1352";
        return -1;
    }
    if (cfg->s2 < 0 || cfg->s2 > AWG_PACKET_BUF_SIZE - WG_RESP_SIZE) {
        *err_msg = "AWG_S2: must be between 0 and 1408";
        return -1;
    }
    if (cfg->s3 < 0 || cfg->s3 > AWG_PACKET_BUF_SIZE - WG_COOKIE_SIZE) {
        *err_msg = "AWG_S3: must be between 0 and 1436";
        return -1;
    }
    if (cfg->s4 < 0 || cfg->s4 > AWG_PACKET_HEADROOM) {
        *err_msg = "AWG_S4: must be between 0 and 256";
        return -1;
    }
    if (hrange_overlaps(&cfg->h1, &cfg->h2) ||
        hrange_overlaps(&cfg->h1, &cfg->h3) ||
        hrange_overlaps(&cfg->h1, &cfg->h4) ||
        hrange_overlaps(&cfg->h2, &cfg->h3) ||
        hrange_overlaps(&cfg->h2, &cfg->h4) ||
        hrange_overlaps(&cfg->h3, &cfg->h4)) {
        *err_msg = "AWG_H1..AWG_H4: ranges must not overlap";
        return -1;
    }

    *err_msg = NULL;
    return 0;
}

void config_compute(awg_config_t *cfg) {
    static const uint8_t z[32] = {0};
    cfg->has_server_pub = memcmp(cfg->server_pub, z, 32) != 0;
    cfg->has_client_pub = memcmp(cfg->client_pub, z, 32) != 0;

    compute_mac1_key(cfg->server_pub, cfg->mac1key_server);
    compute_mac1_key(cfg->client_pub, cfg->mac1key_client);
    for (int i = 0; i < cfg->server_peer_count; i++)
        compute_mac1_key(cfg->server_peer_pubs[i], cfg->server_peer_mac1keys[i]);

    cfg->h4_fixed = cfg->h4.min;
    cfg->h4_noop = (cfg->h4.min == WG_TRANSPORT_DATA &&
                    cfg->h4.max == WG_TRANSPORT_DATA && cfg->s4 == 0);
    cfg->init_total = cfg->s1 + WG_INIT_SIZE;
    cfg->resp_total = cfg->s2 + WG_RESP_SIZE;
    cfg->cookie_total = cfg->s3 + WG_COOKIE_SIZE;

    int tmin = cfg->s4 + WG_TRANSPORT_MIN;
    cfg->transport_size_ambiguous =
        (cfg->init_total >= tmin) ||
        (cfg->resp_total >= tmin) ||
        (cfg->cookie_total >= tmin);

    if (cfg->mode == AWG_MODE_NORMAL) {
        cfg->mac1key_out = cfg->has_server_pub ? cfg->mac1key_server : NULL;
        cfg->mac1key_in  = cfg->has_client_pub ? cfg->mac1key_client : NULL;
    } else {
        cfg->mac1key_out = cfg->has_client_pub ? cfg->mac1key_client : NULL;
        cfg->mac1key_in  = cfg->has_server_pub ? cfg->mac1key_server : NULL;
    }
}

static inline uint32_t read32_le(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void write32_le(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

__attribute__((hot))
uint8_t *transform_outbound_with_mac1(uint8_t *buf, int dataoff, int n,
                                      const awg_config_t *cfg,
                                      const uint8_t *mac1key_out,
                                      uint64_t rand_val,
                                      int *out_len, int *sendJunk) {
    const uint8_t *out_key = mac1key_out ? mac1key_out : cfg->mac1key_out;

    *sendJunk = 0;
    if (n < 4) {
        *out_len = n;
        return buf + dataoff;
    }

    uint8_t *data = buf + dataoff;
    uint32_t msgType = read32_le(data);

    if (msgType == WG_HANDSHAKE_INIT && n == WG_INIT_SIZE) {
        write32_le(data, hrange_pick(&cfg->h1, rand_val));
        if (out_key)
            recompute_mac1(data, out_key);
        *sendJunk = (cfg->jc > 0);
        if (cfg->s1 > 0) {
            if (dataoff >= cfg->s1) {
                uint8_t *out = data - cfg->s1;
                fastrand_t tmp; fastrand_init(&tmp, rand_val);
                fastrand_fill(&tmp, out, cfg->s1);
                *out_len = cfg->s1 + n;
                return out;
            }
            /* Headroom insufficient: use static buffer (handshakes are rare) */
            fastrand_t tmp; fastrand_init(&tmp, rand_val);
            fastrand_fill(&tmp, hs_buf, cfg->s1);
            memcpy(hs_buf + cfg->s1, data, n);
            *out_len = cfg->s1 + n;
            return hs_buf;
        }
        *out_len = n;
        return data;
    }

    if (msgType == WG_HANDSHAKE_RESPONSE && n == WG_RESP_SIZE) {
        write32_le(data, hrange_pick(&cfg->h2, rand_val));
        if (out_key)
            recompute_mac1_response(data, out_key);
        if (cfg->s2 > 0) {
            if (dataoff >= cfg->s2) {
                uint8_t *out = data - cfg->s2;
                fastrand_t tmp; fastrand_init(&tmp, rand_val ^ 0x12345);
                fastrand_fill(&tmp, out, cfg->s2);
                *out_len = cfg->s2 + n;
                return out;
            }
            fastrand_t tmp; fastrand_init(&tmp, rand_val ^ 0x12345);
            fastrand_fill(&tmp, hs_buf, cfg->s2);
            memcpy(hs_buf + cfg->s2, data, n);
            *out_len = cfg->s2 + n;
            return hs_buf;
        }
        *out_len = n;
        return data;
    }

    if (msgType == WG_COOKIE_REPLY && n == WG_COOKIE_SIZE) {
        write32_le(data, hrange_pick(&cfg->h3, rand_val));
        if (cfg->s3 > 0) {
            if (dataoff >= cfg->s3) {
                uint8_t *out = data - cfg->s3;
                fastrand_t tmp; fastrand_init(&tmp, rand_val ^ 0x67890);
                fastrand_fill(&tmp, out, cfg->s3);
                *out_len = cfg->s3 + n;
                return out;
            }
            fastrand_t tmp; fastrand_init(&tmp, rand_val ^ 0x67890);
            fastrand_fill(&tmp, hs_buf, cfg->s3);
            memcpy(hs_buf + cfg->s3, data, n);
            *out_len = cfg->s3 + n;
            return hs_buf;
        }
        *out_len = n;
        return data;
    }

    if (msgType == WG_TRANSPORT_DATA && n >= WG_TRANSPORT_MIN) {
        if (cfg->h4_noop) {
            *out_len = n;
            return data;
        }
        if (cfg->h4.min == cfg->h4.max)
            write32_le(data, cfg->h4_fixed);
        else
            write32_le(data, hrange_pick(&cfg->h4, rand_val));
        if (cfg->s4 > 0 && dataoff >= cfg->s4) {
            /* Zero-alloc: use headroom. Caller fills random into headroom. */
            *out_len = cfg->s4 + n;
            return buf + dataoff - cfg->s4;
        }
        *out_len = n;
        return data;
    }

    /* Unknown, pass through */
    *out_len = n;
    return data;
}

__attribute__((hot))
uint8_t *transform_outbound(uint8_t *buf, int dataoff, int n,
                             const awg_config_t *cfg, uint64_t rand_val,
                             int *out_len, int *sendJunk) {
    return transform_outbound_with_mac1(buf, dataoff, n, cfg, NULL,
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

__attribute__((hot))
uint8_t *transform_inbound(uint8_t *buf, int n, const awg_config_t *cfg, int *out_len) {
    if (n < 4) return NULL;

    /* Fast path: identity transform */
    if (cfg->h4_noop) {
        if (read32_le(buf) == WG_TRANSPORT_DATA && n >= WG_TRANSPORT_MIN) {
            *out_len = n;
            return buf;
        }
    }

    /* Size-based dispatch: handshake first, transport last */
    if (n == cfg->init_total) {
        uint32_t h = read32_le(buf + cfg->s1);
        if (hrange_contains(&cfg->h1, h)) {
            write32_le(buf + cfg->s1, WG_HANDSHAKE_INIT);
            if (cfg->mac1key_in)
                recompute_mac1(buf + cfg->s1, cfg->mac1key_in);
            *out_len = n - cfg->s1;
            return buf + cfg->s1;
        }
    }

    if (n == cfg->resp_total) {
        uint32_t h = read32_le(buf + cfg->s2);
        if (hrange_contains(&cfg->h2, h)) {
            write32_le(buf + cfg->s2, WG_HANDSHAKE_RESPONSE);
            if (cfg->mac1key_in)
                recompute_mac1_response(buf + cfg->s2, cfg->mac1key_in);
            *out_len = n - cfg->s2;
            return buf + cfg->s2;
        }
    }

    if (n == cfg->cookie_total) {
        uint32_t h = read32_le(buf + cfg->s3);
        if (hrange_contains(&cfg->h3, h)) {
            write32_le(buf + cfg->s3, WG_COOKIE_REPLY);
            *out_len = n - cfg->s3;
            return buf + cfg->s3;
        }
    }

    /* Transport data: variable size, checked last */
    if (n >= cfg->s4 + WG_TRANSPORT_MIN) {
        uint32_t h = read32_le(buf + cfg->s4);
        if (hrange_contains(&cfg->h4, h)) {
            write32_le(buf + cfg->s4, WG_TRANSPORT_DATA);
            *out_len = n - cfg->s4;
            return buf + cfg->s4;
        }
    }

    return NULL;
}

int generate_junk(const awg_config_t *cfg, uint8_t *junk_buf, int *sizes) {
    if (cfg->jc <= 0 || cfg->jmax <= 0) return 0;

    int jmin = cfg->jmin > 0 ? cfg->jmin : 1;
    int jmax = cfg->jmax >= jmin ? cfg->jmax : jmin;
    int span = jmax - jmin + 1;

    /* junk_buf should already be filled with random data by caller */
    fastrand_t r;
    fastrand_init(&r, read32_le(junk_buf) | 1);

    for (int i = 0; i < cfg->jc; i++) {
        sizes[i] = (span > 1) ? jmin + fastrand_intn(&r, span) : jmin;
    }
    return cfg->jc;
}
