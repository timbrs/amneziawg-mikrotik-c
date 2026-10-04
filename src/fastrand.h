#ifndef AWG_FASTRAND_H
#define AWG_FASTRAND_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t s;
} fastrand_t;

void fastrand_init(fastrand_t *r, uint64_t seed);
uint64_t fastrand_u64(fastrand_t *r);
int fastrand_intn(fastrand_t *r, int n);
void fastrand_fill(fastrand_t *r, void *buf, size_t len);

/* Random bytes that go on the wire in the clear before a handshake (I-packets,
 * junk) come from a ChaCha20 keystream, not from fastrand: xorshift hands out
 * its whole state, so any 8 bytes of its output predict every byte after them.
 * Handshakes are rare, so the cost does not matter. */
typedef struct {
    uint8_t key[32];
    uint8_t nonce[12];
    uint32_t ctr;
} csprng_t;

/* stream tells apart generators that share a key. */
void csprng_init(csprng_t *r, const uint8_t key[32], uint8_t stream);
void csprng_fill(csprng_t *r, void *buf, size_t len);

#endif
