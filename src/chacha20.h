#ifndef AWG_CHACHA20_H
#define AWG_CHACHA20_H

#include <stdint.h>
#include <stddef.h>

/* Minimal ChaCha20 (RFC 8439) keystream generator, used for AWG 3.0 header
 * protection. Only what the proxy needs: a keystream starting at block 0 that
 * is XORed over the first bytes of a packet.
 *
 * Block 0 is materialized by chacha20_init(): every packet needs at least its
 * first 4 bytes (the message type), and transport packets — the hot path —
 * never need more than 16. */

#define CHACHA20_KEY_SIZE   32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

typedef struct {
    uint32_t state[16];                  /* state[12] = block counter */
    uint8_t  block[CHACHA20_BLOCK_SIZE]; /* keystream block currently held */
    uint32_t block_idx;                  /* which block sits in block[] */
    uint32_t type_hash;                  /* keystream[0:4] as LE uint32 */
} chacha20_t;

/* Initialize with key/nonce and materialize keystream block 0. */
void chacha20_init(chacha20_t *c, const uint8_t key[CHACHA20_KEY_SIZE],
                   const uint8_t nonce[CHACHA20_NONCE_SIZE]);

/* XOR len bytes of data with the keystream, starting at stream position 0. */
void chacha20_xor(chacha20_t *c, uint8_t *data, size_t len);

/* First 4 keystream bytes as a little-endian uint32 — the AWG "type hash",
 * XORed against the message type field to decode it without touching the
 * rest of the packet. */
static inline uint32_t chacha20_type_hash(const chacha20_t *c) {
    return c->type_hash;
}

#endif
