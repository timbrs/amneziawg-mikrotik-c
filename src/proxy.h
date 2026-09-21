#ifndef AWG_PROXY_H
#define AWG_PROXY_H

#include "transform.h"
#include "fastrand.h"
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define BUF_SIZE       AWG_PACKET_BUF_SIZE
/* Datagrams one recvmmsg/sendmmsg round carries.
 *
 * 32 is measured, not guessed: on a hAP ax2 (Cortex-A53, 864 MHz) raising it to
 * 128 dropped tunnel throughput from 208 to 127 Mbit/s. The batch buffers
 * (BATCH_SIZE * ~1.7 KB per direction) stop fitting the cache, and the saving
 * on syscalls is spent again on misses. */
#define BATCH_SIZE     32
#define H4_RING_SIZE   4096
#define GRO_BUF_SIZE   65536

/* Session table for server mode */
#define SESSION_TABLE_SIZE 4096
#define SESSION_TABLE_MASK (SESSION_TABLE_SIZE - 1)

/* One client-facing address. The listen leg picks its family once, at bind
 * time, from AWG_LISTEN: an IPv4 host (or none) keeps the socket AF_INET,
 * an IPv6 one makes it AF_INET6 with V6ONLY off. Every address the kernel
 * then hands back has that same family — a v4 client on a v6 socket arrives
 * v4-mapped — so the length is a per-socket constant (p->cli_len) and the
 * hot paths never branch on it. */
typedef union {
    struct sockaddr     sa;
    struct sockaddr_in  v4;
    struct sockaddr_in6 v6;
} cliaddr_t;

static inline socklen_t cliaddr_len(const cliaddr_t *a) {
    return a->sa.sa_family == AF_INET6 ? (socklen_t)sizeof(a->v6)
                                       : (socklen_t)sizeof(a->v4);
}

static inline uint16_t cliaddr_port(const cliaddr_t *a) {
    return a->sa.sa_family == AF_INET6 ? a->v6.sin6_port : a->v4.sin_port;
}

/* Address and port both — this answers "same client?", unlike sa_addr_eq. */
static inline int cliaddr_eq(const cliaddr_t *a, const cliaddr_t *b) {
    if (a->sa.sa_family != b->sa.sa_family) return 0;
    if (a->sa.sa_family == AF_INET6)
        return a->v6.sin6_port == b->v6.sin6_port &&
               memcmp(&a->v6.sin6_addr, &b->v6.sin6_addr,
                      sizeof(struct in6_addr)) == 0;
    return a->v4.sin_port == b->v4.sin_port &&
           a->v4.sin_addr.s_addr == b->v4.sin_addr.s_addr;
}

typedef struct {
    uint32_t sender_index;
    cliaddr_t addr;
    _Atomic uint32_t server_index; /* our WireGuard's index for it, 0 = unknown */
    _Atomic int peer_slot;
    _Atomic int prof;      /* obfuscation profile this client speaks */
    _Atomic int valid;
} session_entry_t;

/* Per-source profile cache (server mode, fallback chain enabled). Direct
 * mapped and advisory only: a miss just costs one extra decode attempt, so it
 * is written by the c2s thread alone and needs no synchronisation. */
#define PROF_CACHE_SIZE 64
#define PROF_CACHE_MASK (PROF_CACHE_SIZE - 1)

typedef struct {
    uint32_t key;   /* 0 = empty */
    uint8_t prof;
} prof_cache_entry_t;

static inline uint32_t prof_cache_key(const cliaddr_t *a) {
    uint32_t k;
    if (a->sa.sa_family == AF_INET6) {
        uint32_t w[4];
        memcpy(w, &a->v6.sin6_addr, sizeof(w));
        k = w[0] ^ w[1] ^ w[2] ^ w[3] ^ ((uint32_t)a->v6.sin6_port << 16)
          ^ (uint32_t)a->v6.sin6_port;
    } else {
        k = (uint32_t)a->v4.sin_addr.s_addr ^ ((uint32_t)a->v4.sin_port << 16)
          ^ (uint32_t)a->v4.sin_port;
    }
    return k ? k : 1u;
}


#ifndef UDP_GRO
#define UDP_GRO        104
#endif
#ifndef UDP_SEGMENT
#define UDP_SEGMENT    103
#endif
/* Hard limits the kernel puts on one segmented send: UDP_MAX_SEGMENTS chunks
 * (udp.h), and a payload that still fits a single IPv4 datagram before it is
 * sliced (65535 - 20 - 8). Exceeding either costs the whole batch with EINVAL
 * or EMSGSIZE, so gso_run_len stops short of both. */
#define GSO_MAX_SEGMENTS  64
#define GSO_MAX_BYTES     65507u
#ifndef IPPROTO_UDP
#define IPPROTO_UDP    17
#endif
#ifndef IP_MTU_DISCOVER
#define IP_MTU_DISCOVER   10
#endif
#ifndef IP_PMTUDISC_DONT
#define IP_PMTUDISC_DONT  0
#endif
#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6      41
#endif
#ifndef IPV6_MTU_DISCOVER
#define IPV6_MTU_DISCOVER  23
#endif
#ifndef IPV6_PMTUDISC_DONT
#define IPV6_PMTUDISC_DONT 0
#endif
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY       26
#endif

/* One resolved remote endpoint. len == 0 means "this family has no record". */
typedef struct {
    struct sockaddr_storage sa;
    socklen_t len;
} awg_addr_t;

/* Ports AWG_REMOTE allows. A server that redirects a whole band of ports to
 * its AmneziaWG port lets the client pick a fresh one per connection, so one
 * blocked port stops being fatal. 32 ranges is well past what such a setup
 * hands out (20 blocks today) and keeps the set a flat, cheap struct. */
#define AWG_MAX_PORT_RANGES 32

typedef struct { uint16_t lo, hi; } port_range_t;   /* lo == hi — single port */

typedef struct {
    port_range_t r[AWG_MAX_PORT_RANGES];
    uint8_t  n;
    uint32_t total;      /* sum of (hi-lo+1) — the weight for a uniform draw */
} portset_t;

typedef struct {
    /* === Hot fields === */
    awg_config_t *cfg;              /* 8B */
    int listen_fd;                  /* 4B */
    _Atomic int remote_fd;          /* 4B */
    _Atomic int remote_fd2;         /* 4B — Happy Eyeballs probe socket, -1 = none */
    _Atomic int stopped;            /* 4B */
    _Atomic int has_client;         /* 4B */
    _Atomic int last_active;        /* 4B */
    _Atomic int last_remote_rx;     /* 4B — set when data received from remote */
    _Atomic int client_init;        /* 4B — client sent a WG handshake init */
    _Atomic int he_reset;           /* 4B — drop the learned family on next dial */
    _Atomic int reconnect_needed;   /* 4B */
    /* First-event flags (one per connection — touched once after start/reconnect) */
    _Atomic uint8_t fe_init_seen;       /* WG handshake init seen from client */
    _Atomic uint8_t fe_init_sent;       /* AWG handshake init sent to remote */
    _Atomic uint8_t fe_remote_pkt;      /* any packet received from remote */
    _Atomic uint8_t fe_resp_received;   /* AWG handshake response received */
    _Atomic uint8_t fe_resp_sent;       /* WG handshake response delivered to client */
    _Atomic uint8_t fe_transport_c2s;   /* first transport packet to remote */
    _Atomic uint8_t fe_transport_s2c;   /* first transport packet to client */
    /* The MTU hint is advice about the config, not about this connection, and
     * the config cannot change while the process runs — so unlike the flags
     * above this one is never reset, or the same warning repeats verbatim on
     * every reconnect. */
    _Atomic uint8_t fe_mtu_hint;
    _Atomic uint8_t fe_frag_warn;   /* fragmentation warning already logged */
    cliaddr_t client_addr;          /* 16B v4 / 28B v6 */
    socklen_t cli_len;              /* 4B — namelen for every client-leg msg */
    int gso_ok;                     /* 4B */
    int gro_enabled;                /* 4B */
    int c2s_headroom;               /* 4B — bytes reserved before recv_c2s data */
    int s2c_headroom;               /* 4B — bytes reserved before recv_s2c data */
    uint16_t h4_idx[AWG_MAX_PROFILES];
    fastrand_t rng;                 /* 8B — cold paths (init, H4 ring) */
    fastrand_t rng_c2s;             /* 8B — c2s thread only */
    fastrand_t rng_s2c;             /* 8B — s2c thread only */

    /* === Warm: batch I/O === */

    /* Batch I/O buffers — c2s direction */
    struct {
        uint8_t bufs[BATCH_SIZE][BUF_SIZE + AWG_PACKET_HEADROOM];
        struct mmsghdr msgs[BATCH_SIZE];
        struct iovec iovecs[BATCH_SIZE];
        cliaddr_t addrs[BATCH_SIZE];
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
        cliaddr_t addrs[BATCH_SIZE];
    } send_s2c;

    /* === Cold: init/reconnect === */
    cliaddr_t listen_addr;
    int listen_family;              /* AF_INET or AF_INET6 */
    /* Remote is the only leg that speaks both families. The socket is
     * connect()ed and every send goes through send(), so nothing on the hot
     * path ever looks at these. */
    awg_addr_t remote;              /* endpoint behind remote_fd */
    awg_addr_t remote_alt;          /* endpoint behind remote_fd2 (probe) */
    char remote_host[256];
    uint16_t remote_port;            /* the port this connection is dialing */
    portset_t remote_ports;          /* every port AWG_REMOTE allows */
    int auto_src_port;
    int local_port;

    /* Learned transport preference: which family to dial first. Read from
     * cfg->state_file at init, updated at most once per run (state_written) so
     * a flapping link cannot turn into a write loop on the router's flash. */
    int prefer6;
    int state_written;

    /* Happy Eyeballs: c2s keeps a copy of the first packet it sends so the
     * probe can replay it on the other family. Published by the release store
     * to he_sent; he_pkt/he_pkt_len are plain fields behind it. he_evfd wakes
     * the probe the moment that happens, so the head start is measured from
     * the send and not from whenever poll() next returns. */
    _Atomic int he_sent;
    uint64_t he_sent_ms;
    int he_pkt_len;
    int he_evfd;
    uint8_t he_pkt[BUF_SIZE + AWG_PACKET_HEADROOM];

    uint32_t cps_counter;
    uint8_t *junk_buf;
    int *junk_sizes;

    int signal_fd;
    int timer_fd;

    /* Spin-drain budget in force, in microseconds. Lives here rather than in
     * cfg because the self-tuning controller rewrites it while the reader
     * threads are running. */
    _Atomic int spin_us;

    /* Throughput/loss accounting. Bumped once per batch, not per packet, so
     * the hot path pays one relaxed add per recvmmsg/sendmmsg round. A packet
     * counted in rx but not in tx was dropped by us — that is the number the
     * kernel counters cannot show, because the drop happens in userspace when
     * a send returns EAGAIN/ENOBUFS and the rest of the batch is abandoned.
     *
     * 32 bits, not 64: ARMv5 has no 64-bit atomic instruction, so a `_Atomic
     * unsigned long long` there turns into calls to libatomic that a static
     * build does not link, and the armv5 target stops building altogether.
     * Nothing is lost — only differences between samples are ever used, and
     * unsigned subtraction stays correct across the wrap. */
    _Atomic uint32_t st_c2s_rx;
    _Atomic uint32_t st_c2s_tx;
    _Atomic uint32_t st_c2s_drop;
    _Atomic uint32_t st_s2c_rx;
    _Atomic uint32_t st_s2c_tx;
    _Atomic uint32_t st_s2c_drop;

    /* Segment offload, per direction: how many sendmsg calls carried a
     * coalesced run, and how many packets those runs held. segs/msgs is the
     * average run length — 1.0 means the offload is doing nothing, and until
     * these existed there was no way to tell that from the outside. */
    _Atomic uint32_t st_c2s_gso_msgs;
    _Atomic uint32_t st_c2s_gso_segs;
    _Atomic uint32_t st_s2c_gso_msgs;
    _Atomic uint32_t st_s2c_gso_segs;

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
    cliaddr_t gro_addr_c2s;
    int gro_enabled_c2s;

    /* === Large cold arrays === */
    prof_cache_entry_t prof_cache[PROF_CACHE_SIZE];
    uint32_t h4_ring[AWG_MAX_PROFILES][H4_RING_SIZE];
    session_entry_t sessions[SESSION_TABLE_SIZE];
    /* Server mode: our WireGuard's index -> session slot + 1, see
     * session_set_server_index(). */
    _Atomic uint32_t sidx_map[SESSION_TABLE_SIZE];

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

static inline session_entry_t *session_put_prof(proxy_t *p, uint32_t index,
                                                const cliaddr_t *addr, int prof) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (!atomic_load_explicit(&p->sessions[s].valid, memory_order_acquire) ||
            p->sessions[s].sender_index == index) {
            int preserve_peer = atomic_load_explicit(&p->sessions[s].valid, memory_order_relaxed) &&
                                p->sessions[s].sender_index == index;
            p->sessions[s].sender_index = index;
            p->sessions[s].addr = *addr;
            if (!preserve_peer) {
                atomic_store_explicit(&p->sessions[s].peer_slot, -1, memory_order_relaxed);
                atomic_store_explicit(&p->sessions[s].server_index, 0, memory_order_relaxed);
            }
            atomic_store_explicit(&p->sessions[s].prof, prof, memory_order_relaxed);
            atomic_store_explicit(&p->sessions[s].valid, 1, memory_order_release);
            return &p->sessions[s];
        }
    }
    p->sessions[slot].sender_index = index;
    p->sessions[slot].addr = *addr;
    atomic_store_explicit(&p->sessions[slot].peer_slot, -1, memory_order_relaxed);
    atomic_store_explicit(&p->sessions[slot].server_index, 0, memory_order_relaxed);
    atomic_store_explicit(&p->sessions[slot].prof, prof, memory_order_relaxed);
    atomic_store_explicit(&p->sessions[slot].valid, 1, memory_order_release);
    return &p->sessions[slot];
}

static inline void session_put(proxy_t *p, uint32_t index, const cliaddr_t *addr) {
    session_put_prof(p, index, addr, 0);
}

static inline cliaddr_t *session_get(proxy_t *p, uint32_t index) {
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
        } else if (!cliaddr_eq(&found->addr, &p->sessions[i].addr)) {
            return NULL; /* multiple clients */
        }
    }
    return found;
}

/* Find client address when only one unique client exists in session table */
static inline cliaddr_t *session_find_sole_client(proxy_t *p) {
    session_entry_t *entry = session_find_sole_entry(p);
    return entry ? &entry->addr : NULL;
}

/* First live entry belonging to a peer. Used to aim a server-initiated
 * handshake once its MAC1 has named the peer. */
static inline session_entry_t *session_find_by_peer(proxy_t *p, int peer_slot) {
    if (peer_slot < 0) return NULL;
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        if (!atomic_load_explicit(&p->sessions[i].valid, memory_order_acquire))
            continue;
        if (atomic_load_explicit(&p->sessions[i].peer_slot, memory_order_relaxed)
                == peer_slot)
            return &p->sessions[i];
    }
    return NULL;
}

/* Retire the entries a peer left at addresses it no longer uses.
 *
 * Nothing else ever clears this table, and a client that comes back from a new
 * address (a DHCPv6 lease that renewed into a different one, say) simply adds
 * entries beside the old ones. Two entries with different addresses stop
 * looking like one client, so session_find_sole_entry() returns NULL and every
 * server-initiated handshake is dropped from then on — permanently, since
 * nothing expires. Once a handshake has identified the peer, its entries at
 * *other* addresses are provably dead: that client is not there any more.
 * Entries at the current address are left alone, because a rekey legitimately
 * keeps the previous session alive for packets still in flight. */
static inline void session_drop_moved_peer(proxy_t *p, int peer_slot,
                                           const cliaddr_t *cur) {
    if (peer_slot < 0 || !cur) return;
    for (int i = 0; i < SESSION_TABLE_SIZE; i++) {
        if (!atomic_load_explicit(&p->sessions[i].valid, memory_order_acquire))
            continue;
        if (atomic_load_explicit(&p->sessions[i].peer_slot, memory_order_relaxed)
                != peer_slot)
            continue;
        if (cliaddr_eq(&p->sessions[i].addr, cur))
            continue;
        atomic_store_explicit(&p->sessions[i].valid, 0, memory_order_release);
    }
}

/* Server mode: remember the index our WireGuard gave this session (the
 * sender_index of its handshake response). A client's transport carries that
 * index, never its own, so it is the only handle a client that moved can be
 * recognised by. The map is direct-mapped and advisory: a lost or stale slot
 * only means that session waits for its next handshake to be found again. */
static inline void session_set_server_index(proxy_t *p, session_entry_t *e,
                                            uint32_t sidx) {
    if (!e || !sidx) return;
    atomic_store_explicit(&e->server_index, sidx, memory_order_relaxed);
    atomic_store_explicit(&p->sidx_map[sidx & SESSION_TABLE_MASK],
                          (uint32_t)(e - p->sessions) + 1, memory_order_release);
}

static inline session_entry_t *session_get_by_server_index(proxy_t *p,
                                                           uint32_t sidx) {
    if (!sidx) return NULL;
    uint32_t ref = atomic_load_explicit(&p->sidx_map[sidx & SESSION_TABLE_MASK],
                                        memory_order_acquire);
    if (!ref) return NULL;
    session_entry_t *e = &p->sessions[ref - 1];
    if (!atomic_load_explicit(&e->valid, memory_order_acquire) ||
        atomic_load_explicit(&e->server_index, memory_order_relaxed) != sidx)
        return NULL;
    return e;
}

/* A client's transport for session sidx came from `from`. If the session still
 * points elsewhere, the client moved: its NAT rebound, or its own proxy
 * restarted onto a new port. Follow it, as WireGuard follows a roaming peer —
 * otherwise every reply keeps going to the dead address until the client's
 * next handshake, up to two minutes later. Every entry at the old address
 * moves too: they are the same client (a rekey keeps the previous session
 * alive), and leaving them behind would make one client look like two.
 *
 * Unlike WireGuard, the proxy cannot authenticate the packet, so whoever knows
 * a live session index can redirect its replies. They stay encrypted, and the
 * session comes back with the client's next packet.
 *
 * Returns 1 when the address changed. The c2s thread is the only writer of
 * addr; a reply read mid-update may go out once to a half-written address, as
 * with any in-place update of this table. */
static inline int session_follow_client(proxy_t *p, uint32_t sidx,
                                        const cliaddr_t *from) {
    session_entry_t *e = session_get_by_server_index(p, sidx);
    if (!e || cliaddr_eq(&e->addr, from)) return 0;
    cliaddr_t old = e->addr;
    for (int i = 0; i < SESSION_TABLE_SIZE; i++)
        if (atomic_load_explicit(&p->sessions[i].valid, memory_order_acquire) &&
            cliaddr_eq(&p->sessions[i].addr, &old))
            p->sessions[i].addr = *from;
    return 1;
}

/* Re-check DNS records (A and AAAA) for host: 0 = cur still present,
 * 1 = cur gone, -1 = resolve error. Walks every record to tolerate
 * round-robin DNS; a record matches only when family and address both do. */
int resolve_addr_check(const char *host, const struct sockaddr *cur);

/* Persisted transport preference — a single byte, '6' or '4'. Anything else
 * (missing file, empty path, unreadable) reads as "no preference" = 0, which
 * keeps the stock IPv4-first Happy Eyeballs order. */
int state_read_prefer6(const char *path);
int state_write_prefer6(const char *path, int prefer6);

/* Split "host:port" / "[v6addr]:port". A bare IPv6 literal is rejected: the
 * port is mandatory, so brackets are the only unambiguous form. */
int parse_host_port(const char *s, char *host, int hostmax, uint16_t *port);

/* Same split, but the part after the colon is a set: "443", "443,8080",
 * "20150-20299,21500-21649". A comma cannot appear in a host — not in an IPv4
 * literal, not in a name, not inside brackets — so the two never collide.
 * Rejects an empty token, a non-digit, port 0, a port above 65535, a reversed
 * range and more than AWG_MAX_PORT_RANGES tokens. */
int parse_host_ports(const char *s, char *host, int hostmax, portset_t *ps);

/* Draw a port uniformly over the ports of the set, not over its ranges, so a
 * 150-port block does not weigh the same as a single port. `avoid` (the port
 * just abandoned) gets one redraw. */
uint16_t portset_pick(const portset_t *ps, fastrand_t *rng, uint16_t avoid);

/* Largest WireGuard MTU whose full-size transport packet still fits a
 * 1500-byte path:
 *   IP(20|40) + UDP(8) + S4 + WG hdr(16) + round_up(mtu,16) + tag(16) <= 1500
 * The IPv6 header is 20 bytes longer, so the same config needs a lower MTU —
 * the reason wg-quick picks 1420 for IPv4 and 1400 for IPv6. */
static inline int awg_max_wg_mtu(int s4, int ipv6) {
    int room = (ipv6 ? 1420 : 1440) - s4;
    if (room < 0) room = 0;
    return (room / 16) * 16;
}

/* Segment size of a coalesced UDP read, 0 when the kernel did not coalesce. */
int gro_seg_size(const struct msghdr *hdr);

/* How many packets from the head of a batch one UDP_SEGMENT send can carry;
 * 0 when nothing can be coalesced. addrs non-NULL splits the run by
 * destination (unconnected socket). */
int gso_run_len(const struct iovec *iov, const cliaddr_t *addrs, int count);

/* Initialize proxy. Returns 0 on success. */
int proxy_init(proxy_t *p, awg_config_t *cfg,
               const char *listen_str, const char *remote_str, int src_port);

/* Run proxy event loop. Blocks until signal or error. Returns 0 on clean shutdown. */
int proxy_run(proxy_t *p);

#endif
