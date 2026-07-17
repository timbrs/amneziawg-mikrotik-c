#ifndef AWG_PROXY_H
#define AWG_PROXY_H

#include "transform.h"
#include "fastrand.h"
#include <stdint.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUF_SIZE       AWG_PACKET_BUF_SIZE
#define BATCH_SIZE     32
#define H4_RING_SIZE   4096
#define GRO_BUF_SIZE   65536

/* Session table for server mode */
#define SESSION_TABLE_SIZE 4096
#define SESSION_TABLE_MASK (SESSION_TABLE_SIZE - 1)

typedef struct {
    uint32_t sender_index;
    struct sockaddr_in addr;
    _Atomic int peer_slot;
    _Atomic int valid;
} session_entry_t;


#ifndef UDP_GRO
#define UDP_GRO        104
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT    103
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP    17
#endif
#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER   10
#endif
#ifndef IP_PMTUDISC_DONT
#define IP_PMTUDISC_DONT  0
#endif

typedef struct {
    /* === Hot fields === */
    awg_config_t *cfg;              /* 8B */
    int listen_fd;                  /* 4B */
    _Atomic int remote_fd;          /* 4B */
    _Atomic int stopped;            /* 4B */
    _Atomic int has_client;         /* 4B */
    _Atomic int last_active;        /* 4B */
    _Atomic int last_remote_rx;     /* 4B — set when data received from remote */
    _Atomic int reconnect_needed;   /* 4B */
    /* First-event flags (one per connection — touched once after start/reconnect) */
    _Atomic uint8_t fe_init_seen;       /* WG handshake init seen from client */
    _Atomic uint8_t fe_init_sent;       /* AWG handshake init sent to remote */
    _Atomic uint8_t fe_remote_pkt;      /* any packet received from remote */
    _Atomic uint8_t fe_resp_received;   /* AWG handshake response received */
    _Atomic uint8_t fe_resp_sent;       /* WG handshake response delivered to client */
    _Atomic uint8_t fe_transport_c2s;   /* first transport packet to remote */
    _Atomic uint8_t fe_transport_s2c;   /* first transport packet to client */
    uint8_t _pad_fe;
    struct sockaddr_in client_addr; /* 16B */
    int gso_ok;                     /* 4B */
    int gro_enabled;                /* 4B */
    uint16_t h4_idx;                /* 2B */
    uint16_t _pad0;                 /* 2B */
    fastrand_t rng;                 /* 8B */

    /* === Warm: batch I/O === */

    /* Batch I/O buffers — c2s direction */
    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + AWG_PACKET_HEADROOM];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        struct sockaddr_in addrs[BATCH_SIZE];
    } recv_c2s;

    struct {
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
    } send_c2s;

    /* Batch I/O buffers — s2c direction (non-GRO path) */
    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + AWG_PACKET_HEADROOM];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
    } recv_s2c;

    struct {
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        struct sockaddr_in addrs[BATCH_SIZE];
    } send_s2c;

    /* === Cold: init/reconnect === */
    struct sockaddr_in listen_addr;
    struct sockaddr_in remote_addr;
    char remote_host[256];
    uint16_t remote_port;
    int auto_src_port;
    int local_port;

    uint32_t cps_counter;
    uint8_t *junk_buf;
    int *junk_sizes;

    int signal_fd;
    int timer_fd;

    uint8_t cps_bufs[5][1500];
    int cps_lens[5];

    /* GRO state — s2c */
    uint8_t gro_buf[GRO_BUF_SIZE];
    struct iovec gro_iov;
    struct msghdr gro_hdr;
    uint8_t gro_cmsg[32];

    /* GRO state — c2s */
    uint8_t gro_buf_c2s[GRO_BUF_SIZE];
    struct iovec gro_iov_c2s;
    struct msghdr gro_hdr_c2s;
    uint8_t gro_cmsg_c2s[32];
    struct sockaddr_in gro_addr_c2s;
    int gro_enabled_c2s;

    /* === Large cold arrays === */
    uint32_t h4_ring[H4_RING_SIZE];
    session_entry_t sessions[SESSION_TABLE_SIZE];

} proxy_t;

/* Session table operations */
static inline session_entry_t *session_get_entry(proxy_t *p, uint32_t index) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (atomic_load_explicit(&p->sessions[s].valid, memory_order_acquire) &&
            p->sessions[s].sender_index == index)
            return &p->sessions[s];
    }
    return NULL;
}

static inline void session_put(proxy_t *p, uint32_t index, struct sockaddr_in *addr) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (!atomic_load_explicit(&p->sessions[s].valid, memory_order_acquire) ||
            p->sessions[s].sender_index == index) {
            int preserve_peer = atomic_load_explicit(&p->sessions[s].valid, memory_order_relaxed) &&
                                p->sessions[s].sender_index == index;
            p->sessions[s].sender_index = index;
            p->sessions[s].addr = *addr;
            if (!preserve_peer)
                atomic_store_explicit(&p->sessions[s].peer_slot, -1, memory_order_relaxed);
            atomic_store_explicit(&p->sessions[s].valid, 1, memory_order_release);
            return;
        }
    }
    p->sessions[slot].sender_index = index;
    p->sessions[slot].addr = *addr;
    atomic_store_explicit(&p->sessions[slot].peer_slot, -1, memory_order_relaxed);
    atomic_store_explicit(&p->sessions[slot].valid, 1, memory_order_release);
}

static inline struct sockaddr_in *session_get(proxy_t *p, uint32_t index) {
    session_entry_t *entry = session_get_entry(p, index);
    return entry ? &entry->addr : NULL;
}

static inline void session_set_peer_slot(session_entry_t *entry, int peer_slot) {
    if (entry)
        atomic_store_explicit(&entry->peer_slot, peer_slot, memory_order_relaxed);
}

static inline int session_get_peer_slot(session_entry_t *entry) {
    return entry ? atomic_load_explicit(&entry->peer_slot, memory_order_relaxed) : -1;
}

/* Find client entry when only one unique client exists in session table */
static inline session_entry_t *session_find_sole_entry(proxy_t *p) {
    session_entry_t *found = NULL;
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        if (!atomic_load_explicit(&p->sessions[i].valid, memory_order_acquire))
            continue;
        if (!found) {
            found = &p->sessions[i];
        } else if (found->addr.sin_addr.s_addr != p->sessions[i].addr.sin_addr.s_addr ||
                   found->addr.sin_port != p->sessions[i].addr.sin_port) {
            return NULL; /* multiple clients */
        }
    }
    return found;
}

/* Find client address when only one unique client exists in session table */
static inline struct sockaddr_in *session_find_sole_client(proxy_t *p) {
    session_entry_t *entry = session_find_sole_entry(p);
    return entry ? &entry->addr : NULL;
}

/* Re-check DNS A records for host: 0 = cur still present, 1 = cur gone,
 * -1 = resolve error. Walks every record to tolerate round-robin DNS. */
int resolve_addr_check(const char *host, const struct in_addr *cur);

/* Initialize proxy. Returns 0 on success. */
int proxy_init(proxy_t *p, awg_config_t *cfg,
               const char *listen_str, const char *remote_str, int src_port);

/* Run proxy event loop. Blocks until signal or error. Returns 0 on clean shutdown. */
int proxy_run(proxy_t *p);

#endif
