#include "fastrand.h"
#include "chacha20.h"
#include <string.h>

void fastrand_init(fastrand_t *r, uint64_t seed) {
    r->s = seed ? seed : 1;
}

uint64_t fastrand_u64(fastrand_t *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}

int fastrand_intn(fastrand_t *r, int n) {
    return (int)(fastrand_u64(r) % (uint64_t)n);
}

void fastrand_fill(fastrand_t *r, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t i;
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t v = fastrand_u64(r);
        memcpy(p + i, &v, 8);
    }
    if (i < len) {
        uint64_t v = fastrand_u64(r);
        memcpy(p + i, &v, len - i);
    }
}

void csprng_init(csprng_t *r, const uint8_t key[32], uint8_t stream) {
    memcpy(r->key, key, sizeof(r->key));
    memset(r->nonce, 0, sizeof(r->nonce));
    r->nonce[0] = stream;
    r->ctr = 0;
}

/* A partial last block is thrown away: the next call starts on a fresh one. */
void csprng_fill(csprng_t *r, void *buf, size_t len) {
    chacha20_keystream(r->key, r->nonce, r->ctr, (uint8_t *)buf, (int)len);
    r->ctr += (uint32_t)((len + CHACHA20_BLOCK_SIZE - 1) / CHACHA20_BLOCK_SIZE);
}
