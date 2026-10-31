#ifndef AWG_NORMAL_SESSIONS_H
#define AWG_NORMAL_SESSIONS_H
/* Included after cliaddr_t. No socket or worker per client. */
#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#define NORMAL_SLOTS 4096u
#define NORMAL_IDLE 300u

typedef struct {
    cliaddr_t addr;
    uint32_t local, wire;
    uint64_t seen, order;
    int peer, used;
} normal_session_t;
typedef struct {
    pthread_mutex_t lock;
    uint32_t next;
    uint64_t order;
    normal_session_t slots[NORMAL_SLOTS];
} normal_sessions_t;

static inline uint64_t normal_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec;
}

/* Handshakes only: allocate an independent wire index even when two native
 * clients chose the same local index. Transport lookup is O(1). Never evict
 * a live entry; exhaustion drops a new handshake, not another client's data. */
static inline int normal_register(normal_sessions_t *t, const cliaddr_t *addr,
                                   uint32_t local, int inherit_peer, uint32_t *wire) {
    uint64_t now = normal_now(), order = 0;
    int peer = -1, found = 0;
    pthread_mutex_lock(&t->lock);
    for (unsigned i = 0; i < NORMAL_SLOTS; i++) {
        normal_session_t *s = &t->slots[i];
        if (!s->used || now - s->seen >= NORMAL_IDLE || !cliaddr_eq(&s->addr, addr)) continue;
        if (s->local == local) {
            s->seen = now;
            *wire = s->wire;
            found = 1;
            goto done;
        }
        if (s->peer >= 0 && s->order > order) { peer = s->peer; order = s->order; }
    }
    for (unsigned i = 0; i < NORMAL_SLOTS; i++) {
        uint32_t id = ++t->next;
        if (!id) id = ++t->next;
        normal_session_t *s = &t->slots[id & (NORMAL_SLOTS - 1)];
        if (s->used && now - s->seen < NORMAL_IDLE) continue;
        *s = (normal_session_t){ .addr = *addr, .local = local, .wire = id,
            .seen = now, .order = ++t->order, .peer = inherit_peer ? peer : -1, .used = 1 };
        *wire = id;
        found = 1;
        break;
    }
done:
    pthread_mutex_unlock(&t->lock);
    return found;
}

/* peer >= 0 binds a validated handshake MAC1 recipient (not authentication).
 * The WG endpoints still authenticate/decrypt everything. */
static inline int normal_lookup(normal_sessions_t *t, uint32_t wire, int peer,
                                 cliaddr_t *addr, uint32_t *local) {
    int found = 0;
    uint64_t now = normal_now();
    pthread_mutex_lock(&t->lock);
    normal_session_t *s = &t->slots[wire & (NORMAL_SLOTS - 1)];
    if (s->used && s->wire == wire && now - s->seen < NORMAL_IDLE &&
        (peer < 0 || s->peer < 0 || s->peer == peer)) {
        s->seen = now;
        if (peer >= 0) { s->peer = peer; s->order = ++t->order; }
        *addr = s->addr;
        *local = s->local;
        found = 1;
    }
    pthread_mutex_unlock(&t->lock);
    return found;
}

static inline int normal_peer_addr(normal_sessions_t *t, int peer, cliaddr_t *addr) {
    uint64_t now = normal_now(), order = 0;
    pthread_mutex_lock(&t->lock);
    for (unsigned i = 0; i < NORMAL_SLOTS; i++) {
        normal_session_t *s = &t->slots[i];
        if (s->used && s->peer == peer && now - s->seen < NORMAL_IDLE && s->order > order) {
            *addr = s->addr;
            order = s->order;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return order != 0;
}
#endif
