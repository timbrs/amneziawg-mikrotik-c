#ifndef AWG_BLAKE2S_H
#define AWG_BLAKE2S_H

#include <stdint.h>
#include <stddef.h>

/* BLAKE2s-256 (unkeyed, 32-byte output) */
void blake2s_256(const void *data, size_t len, uint8_t out[32]);

/* BLAKE2s-128 MAC (keyed, 16-byte output) */
void blake2s_128mac(const uint8_t key[32], const void *data, size_t len, uint8_t out[16]);

/* Derive MAC1 key: BLAKE2s-256("mac1----" || pubkey) */
void compute_mac1_key(const uint8_t pubkey[32], uint8_t mac1key[32]);

/* Recompute MAC1 in handshake init (148 bytes). MAC1 at [116:132], covers [0:116]. */
void recompute_mac1(uint8_t *buf, const uint8_t mac1key[32]);

/* Recompute MAC1 in handshake response (92 bytes). MAC1 at [60:76], covers [0:60]. */
void recompute_mac1_response(uint8_t *buf, const uint8_t mac1key[32]);

#endif
