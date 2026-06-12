#ifndef AWG_CONCURRENCY_H
#define AWG_CONCURRENCY_H

/*
 * Cross-thread primitives shared between the c2s and s2c proxy threads.
 *
 * The proxy has exactly two hot threads and two pieces of mutable state
 * that cross between them; both used to be touched without a memory-safe
 * protocol. These helpers make those accesses race-free:
 *
 *   client_slot_t — a single-writer seqlock around the client sockaddr.
 *     c2s is the sole writer (it learns/updates the client address); s2c
 *     reads it for every reply. A plain struct read could tear while c2s
 *     rewrites the 16 bytes on a client port roam, sending a reply to a
 *     spliced (new addr + old port) destination. The seqlock guarantees
 *     the reader only ever returns a value that was written atomically.
 *
 *   fd_retire_t — one-generation deferred close of the remote fd. s2c's
 *     do_reconnect() must not close the old fd immediately: c2s may have
 *     just loaded it and be about to send on it, and the kernel could
 *     reuse the freed number. Retiring defers the close by one reconnect
 *     generation — by then c2s has long since reloaded remote_fd — so a
 *     send can never land on a closed-and-reused descriptor.
 *
 * Single writer per primitive (c2s for the slot, s2c for the retire list),
 * so neither needs a lock; only C11 acquire/release ordering.
 */

#include <stdatomic.h>
#include <unistd.h>
#include <string.h>
#include <netinet/in.h>

/* ---- client_slot_t: single-writer seqlock ---- */

typedef struct {
    _Atomic unsigned seq;      /* even = stable, odd = write in progress */
    _Atomic int has_value;     /* 0 until the first write publishes an addr */
    struct sockaddr_in addr;   /* guarded by seq */
} client_slot_t;

static inline void client_slot_init(client_slot_t *cs) {
    atomic_store_explicit(&cs->seq, 0, memory_order_relaxed);
    atomic_store_explicit(&cs->has_value, 0, memory_order_relaxed);
    memset(&cs->addr, 0, sizeof(cs->addr));
}

/* Sole writer (c2s). Publishes addr so concurrent readers never tear. */
static inline void client_slot_write(client_slot_t *cs, const struct sockaddr_in *addr) {
    unsigned s = atomic_load_explicit(&cs->seq, memory_order_relaxed);
    /* Enter write: make seq odd, then fence so the body cannot float up. */
    atomic_store_explicit(&cs->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    cs->addr = *addr;
    /* Leave write: store-release makes the body visible before seq goes even. */
    atomic_store_explicit(&cs->seq, s + 2, memory_order_release);
    atomic_store_explicit(&cs->has_value, 1, memory_order_release);
}

/* Reader (s2c). Returns 1 and fills *out with a consistent snapshot, or 0
 * if no address has been published yet. */
static inline int client_slot_read(client_slot_t *cs, struct sockaddr_in *out) {
    if (!atomic_load_explicit(&cs->has_value, memory_order_acquire))
        return 0;
    for (;;) {
        unsigned s1 = atomic_load_explicit(&cs->seq, memory_order_acquire);
        if (s1 & 1u)
            continue;                 /* writer mid-update, retry */
        *out = cs->addr;
        atomic_thread_fence(memory_order_acquire);
        unsigned s2 = atomic_load_explicit(&cs->seq, memory_order_relaxed);
        if (s1 == s2)
            return 1;                 /* no write straddled the copy */
    }
}

/* ---- fd_retire_t: one-generation deferred close ---- */

typedef struct {
    int retained;   /* fd kept alive one extra generation, or -1 */
} fd_retire_t;

static inline void fd_retire_init(fd_retire_t *r) {
    r->retained = -1;
}

/* Retire an fd. Closes the fd retired in the previous call (now safely
 * unreferenced) and keeps the new one alive for one more generation.
 * A negative fd is a no-op and does not disturb the retained generation. */
static inline void fd_retire_push(fd_retire_t *r, int fd) {
    if (fd < 0)
        return;
    if (r->retained >= 0)
        close(r->retained);
    r->retained = fd;
}

/* Close the last retained fd. Call once on shutdown after threads join. */
static inline void fd_retire_drain(fd_retire_t *r) {
    if (r->retained >= 0) {
        close(r->retained);
        r->retained = -1;
    }
}

#endif /* AWG_CONCURRENCY_H */
