#include "blake2s.h"
#include <string.h>

static const uint32_t blake2s_iv[8] = {
    0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
    0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19,
};

static const uint8_t blake2s_sigma[10][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},
    {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},
    {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},
    {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},
    {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},
};

static inline uint32_t rotr32(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint32_t load32_le(const void *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline void store32_le(void *p, uint32_t v) {
    memcpy(p, &v, 4);
}

#define G(v, a, b, c, d, x, y) do { \
    v[a] += v[b] + (x); \
    v[d] = rotr32(v[d] ^ v[a], 16); \
    v[c] += v[d]; \
    v[b] = rotr32(v[b] ^ v[c], 12); \
    v[a] += v[b] + (y); \
    v[d] = rotr32(v[d] ^ v[a], 8); \
    v[c] += v[d]; \
    v[b] = rotr32(v[b] ^ v[c], 7); \
} while(0)

static void blake2s_compress(uint32_t h[8], const uint8_t block[64],
                              uint32_t t0, uint32_t t1, int last) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = load32_le(block + i * 4);

    uint32_t v[16] = {
        h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
        blake2s_iv[0], blake2s_iv[1], blake2s_iv[2], blake2s_iv[3],
        t0 ^ blake2s_iv[4], t1 ^ blake2s_iv[5], blake2s_iv[6], blake2s_iv[7],
    };
    if (last) v[14] ^= 0xFFFFFFFF;

    for (int i = 0; i < 10; i++) {
        const uint8_t *s = blake2s_sigma[i];
        G(v, 0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(v, 1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(v, 2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(v, 3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(v, 0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(v, 2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(v, 3, 4, 9, 14, m[s[14]], m[s[15]]);
    }

    for (int i = 0; i < 8; i++)
        h[i] ^= v[i] ^ v[i + 8];
}

typedef struct {
    uint32_t h[8];
    uint32_t t0, t1;
    uint8_t buf[64];
    int buflen;
    int nn;
} blake2s_state;

static void blake2s_init(blake2s_state *st, int nn, const uint8_t *key, int kk) {
    memcpy(st->h, blake2s_iv, 32);
    st->h[0] ^= 0x01010000 | ((uint32_t)kk << 8) | (uint32_t)nn;
    st->t0 = st->t1 = 0;
    st->buflen = 0;
    st->nn = nn;
    memset(st->buf, 0, 64);
    if (kk > 0) {
        memcpy(st->buf, key, kk);
        st->buflen = 64;
    }
}

static void blake2s_update(blake2s_state *st, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    if (len == 0) return;

    int left = st->buflen;
    int fill = 64 - left;

    if ((int)len > fill) {
        memcpy(st->buf + left, p, fill);
        st->t0 += 64;
        if (st->t0 < 64) st->t1++;
        blake2s_compress(st->h, st->buf, st->t0, st->t1, 0);
        p += fill;
        len -= fill;
        st->buflen = 0;

        while (len > 64) {
            st->t0 += 64;
            if (st->t0 < 64) st->t1++;
            blake2s_compress(st->h, p, st->t0, st->t1, 0);
            p += 64;
            len -= 64;
        }
    }

    memcpy(st->buf + st->buflen, p, len);
    st->buflen += (int)len;
}

static void blake2s_final(blake2s_state *st, uint8_t *out) {
    uint32_t n = (uint32_t)st->buflen;
    st->t0 += n;
    if (st->t0 < n) st->t1++;
    memset(st->buf + st->buflen, 0, 64 - st->buflen);
    blake2s_compress(st->h, st->buf, st->t0, st->t1, 1);
    for (int i = 0; i < 8; i++)
        store32_le(out + i * 4, st->h[i]);
}

void blake2s_256(const void *data, size_t len, uint8_t out[32]) {
    blake2s_state st;
    blake2s_init(&st, 32, NULL, 0);
    blake2s_update(&st, data, len);
    blake2s_final(&st, out);
}

void blake2s_128mac(const uint8_t key[32], const void *data, size_t len, uint8_t out[16]) {
    blake2s_state st;
    uint8_t full[32];
    blake2s_init(&st, 16, key, 32);
    blake2s_update(&st, data, len);
    blake2s_final(&st, full);
    memcpy(out, full, 16);
}

void compute_mac1_key(const uint8_t pubkey[32], uint8_t mac1key[32]) {
    uint8_t input[40];
    memcpy(input, "mac1----", 8);
    memcpy(input + 8, pubkey, 32);
    blake2s_256(input, 40, mac1key);
}

void recompute_mac1(uint8_t *buf, const uint8_t mac1key[32]) {
    uint8_t mac1[16];
    blake2s_128mac(mac1key, buf, 116, mac1);
    memcpy(buf + 116, mac1, 16);
}

void recompute_mac1_response(uint8_t *buf, const uint8_t mac1key[32]) {
    uint8_t mac1[16];
    blake2s_128mac(mac1key, buf, 60, mac1);
    memcpy(buf + 60, mac1, 16);
}
