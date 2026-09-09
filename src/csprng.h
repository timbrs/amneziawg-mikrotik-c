#ifndef AWG_CSPRNG_H
#define AWG_CSPRNG_H

#include <stdint.h>
#include <stddef.h>

/* Cryptographically strong random bytes, buffered from the kernel.
 *
 * fastrand (xorshift64) is fine for picking H values or junk sizes, but not for
 * anything an observer sees raw: the packet padding is such a place. With AWG 3.0
 * it doubles as the ChaCha20 nonce, and a predictable padding both fingerprints
 * the proxy and leaks the generator state. amneziawg-go uses crypto/rand here.
 *
 * Bytes are drawn from a per-thread pool refilled from the kernel, so the syscall
 * cost is amortized over hundreds of packets. */

/* Open/verify the entropy source. 0 = ok, -1 = unavailable. Call once at startup
 * for an early, clear failure; csprng_bytes() also initializes lazily. */
int csprng_init(void);

/* Fill out[0..len) with random bytes. Never fails once csprng_init() succeeded. */
void csprng_bytes(uint8_t *out, size_t len);

#endif
