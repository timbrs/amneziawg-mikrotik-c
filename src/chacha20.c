#include "chacha20.h"
#include <string.h>

#define ROTL32(v, n) (uint32_t)(((v) << (n)) | ((v) >> (32 - (n))))

static inline uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#define QROUND(a, b, c, d)          \
    do {                            \
        a += b; d ^= a; d = ROTL32(d, 16); \
        c += d; b ^= c; b = ROTL32(b, 12); \
        a += b; d ^= a; d = ROTL32(d, 8);  \
        c += d; b ^= c; b = ROTL32(b, 7);  \
    } while (0)

/* One keystream block for the given counter. */
static void chacha20_block(const uint32_t state[16], uint32_t counter,
                           uint8_t out[CHACHA20_BLOCK_SIZE]) {
    uint32_t init[16], x[16];

    memcpy(init, state, sizeof(init));
    init[12] = counter;
    memcpy(x, init, sizeof(x));

    for (int i = 0; i < 10; i++) {
        /* column rounds */
        QROUND(x[0], x[4], x[8],  x[12]);
        QROUND(x[1], x[5], x[9],  x[13]);
        QROUND(x[2], x[6], x[10], x[14]);
        QROUND(x[3], x[7], x[11], x[15]);
        /* diagonal rounds */
        QROUND(x[0], x[5], x[10], x[15]);
        QROUND(x[1], x[6], x[11], x[12]);
        QROUND(x[2], x[7], x[8],  x[13]);
        QROUND(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++)
        wr32le(out + i * 4, x[i] + init[i]);
}

void chacha20_init(chacha20_t *c, const uint8_t key[CHACHA20_KEY_SIZE],
                   const uint8_t nonce[CHACHA20_NONCE_SIZE]) {
    /* "expand 32-byte k" */
    c->state[0] = 0x61707865;
    c->state[1] = 0x3320646e;
    c->state[2] = 0x79622d32;
    c->state[3] = 0x6b206574;

    for (int i = 0; i < 8; i++)
        c->state[4 + i] = rd32le(key + i * 4);

    c->state[12] = 0;

    for (int i = 0; i < 3; i++)
        c->state[13 + i] = rd32le(nonce + i * 4);

    chacha20_block(c->state, 0, c->block);
    c->block_idx = 0;
    c->type_hash = rd32le(c->block);
}

void chacha20_xor(chacha20_t *c, uint8_t *data, size_t len) {
    size_t off = 0;
    uint32_t blk = 0;

    while (off < len) {
        if (c->block_idx != blk) {
            chacha20_block(c->state, blk, c->block);
            c->block_idx = blk;
        }

        size_t n = len - off;
        if (n > CHACHA20_BLOCK_SIZE)
            n = CHACHA20_BLOCK_SIZE;

        for (size_t i = 0; i < n; i++)
            data[off + i] ^= c->block[i];

        off += n;
        blk++;
    }
}
