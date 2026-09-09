#include "proxy.h"
#include "cps.h"
#include "log.h"
#include "csprng.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

/* ---- Helpers ---- */

/* One H4 ring per profile: server mode picks H4 per client, so the rings must
 * not be rebuilt behind each other's back. */
static void fill_h4_ring(proxy_t *p, int prof) {
    const hrange_t *h4 = &p->cfg->profiles[prof].h4;
    uint32_t *ring = p->h4_ring[prof];

    if (h4->min == h4->max) {
        uint32_t v = h4->min;
        for (int i = 0; i < H4_RING_SIZE; i++)
            ring[i] = v;
        return;
    }
    for (int i = 0; i < H4_RING_SIZE; i++)
        ring[i] = hrange_pick(h4, fastrand_u64(&p->rng));
}

static void fill_h4_rings(proxy_t *p) {
    for (int i = 0; i < p->cfg->profile_count; i++)
        fill_h4_ring(p, i);
}

static inline uint32_t pick_h4_prof(proxy_t *p, int prof) {
    uint16_t idx = p->h4_idx[prof];
    uint32_t v = p->h4_ring[prof][idx];
    idx = (uint16_t)((idx + 1) & (H4_RING_SIZE - 1));
    p->h4_idx[prof] = idx;
    if (idx == 0)
        fill_h4_ring(p, prof);
    return v;
}

static inline uint32_t pick_h4(proxy_t *p) {
    return pick_h4_prof(p, p->cfg->active_profile);
}

/* Switch the active obfuscation profile. Only called while the tunnel is down
 * (initiator) or on a fresh handshake (responder), so no transport is in
 * flight during the swap. Server mode never uses this — it tracks the profile
 * per client instead. */
static void switch_profile(proxy_t *p, int idx) {
    config_apply_profile(p->cfg, idx);
}

/* --- Per-source profile cache (server mode with a fallback chain) --- */

static inline int profile_for_addr(proxy_t *p, const cliaddr_t *a) {
    if (p->cfg->profile_count == 1) return 0;
    uint32_t k = prof_cache_key(a);
    prof_cache_entry_t *e = &p->prof_cache[(k ^ (k >> 16)) & PROF_CACHE_MASK];
    return e->key == k ? e->prof : p->cfg->active_profile;
}

static inline void profile_remember(proxy_t *p, const cliaddr_t *a, int prof) {
    uint32_t k = prof_cache_key(a);
    prof_cache_entry_t *e = &p->prof_cache[(k ^ (k >> 16)) & PROF_CACHE_MASK];
    e->key = k;
    e->prof = (uint8_t)prof;
}

static int checked_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a)
        return -1;
    *out = a * b;
    return 0;
}

static int junk_layout_sizes(const awg_config_t *cfg,
                             size_t *junk_bytes, size_t *junk_sizes_bytes) {
    if (checked_mul_size((size_t)cfg->jc, (size_t)cfg->jmax, junk_bytes) < 0)
        return -1;
    if (checked_mul_size((size_t)cfg->jc, sizeof(int), junk_sizes_bytes) < 0)
        return -1;
    return 0;
}

/* Address bytes only — the port never participates: the caller always
 * overwrites it with the configured one. */
static int sa_addr_eq(const struct sockaddr *a, const struct sockaddr *b) {
    if (a->sa_family != b->sa_family) return 0;
    if (a->sa_family == AF_INET)
        return ((const struct sockaddr_in *)a)->sin_addr.s_addr ==
               ((const struct sockaddr_in *)b)->sin_addr.s_addr;
    if (a->sa_family == AF_INET6)
        return memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr,
                      &((const struct sockaddr_in6 *)b)->sin6_addr,
                      sizeof(struct in6_addr)) == 0;
    return 0;
}

static void sa_set_port(struct sockaddr_storage *ss, uint16_t port) {
    if (ss->ss_family == AF_INET6)
        ((struct sockaddr_in6 *)ss)->sin6_port = htons(port);
    else
        ((struct sockaddr_in *)ss)->sin_port = htons(port);
}

/* Printable address of a resolved endpoint. buf must hold INET6_ADDRSTRLEN. */
static const char *sa_str(const struct sockaddr *sa, char *buf, size_t buflen) {
    const void *src = (sa->sa_family == AF_INET6)
        ? (const void *)&((const struct sockaddr_in6 *)sa)->sin6_addr
        : (const void *)&((const struct sockaddr_in *)sa)->sin_addr;
    const char *r = inet_ntop(sa->sa_family, src, buf, (socklen_t)buflen);
    return r ? r : "?";
}

/* Resolve host into one A and one AAAA endpoint. Either may come back empty
 * (len == 0); returns -1 only when neither family produced a record. Both are
 * kept so the caller can run the Happy Eyeballs probe over them. */
static int resolve_addr(const char *host, uint16_t port,
                        awg_addr_t *v4, awg_addr_t *v6) {
    memset(v4, 0, sizeof(*v4));
    memset(v6, 0, sizeof(*v6));

    struct sockaddr_in lit4;
    struct sockaddr_in6 lit6;
    if (inet_pton(AF_INET, host, &lit4.sin_addr) == 1) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&v4->sa;
        sa->sin_family = AF_INET;
        sa->sin_addr = lit4.sin_addr;
        sa->sin_port = htons(port);
        v4->len = sizeof(*sa);
        return 0;
    }
    if (inet_pton(AF_INET6, host, &lit6.sin6_addr) == 1) {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *)&v6->sa;
        sa->sin6_family = AF_INET6;
        sa->sin6_addr = lit6.sin6_addr;
        sa->sin6_port = htons(port);
        v6->len = sizeof(*sa);
        return 0;
    }

    log_info2("resolving ", host);

    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res;
    int gai_err = getaddrinfo(host, NULL, &hints, &res);
    if (gai_err != 0) {
        const char *parts[] = {"resolve ", host, ": ", gai_strerror(gai_err)};
        if (g_log_level >= LOG_ERROR) log_msgn("ERROR: ", parts, 4);
        return -1;
    }

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        awg_addr_t *slot = (ai->ai_family == AF_INET6) ? v6 :
                           (ai->ai_family == AF_INET)  ? v4 : NULL;
        if (!slot || slot->len || ai->ai_addrlen > sizeof(slot->sa)) continue;
        memcpy(&slot->sa, ai->ai_addr, ai->ai_addrlen);
        slot->len = (socklen_t)ai->ai_addrlen;
        sa_set_port(&slot->sa, port);
    }
    freeaddrinfo(res);

    if (!v4->len && !v6->len) {
        log_error2("resolve produced no usable record for ", host);
        return -1;
    }

    if (g_log_level >= LOG_INFO) {
        char b4[INET6_ADDRSTRLEN], b6[INET6_ADDRSTRLEN];
        const char *parts[] = { "resolved ", host, " -> ",
            v4->len ? sa_str((struct sockaddr *)&v4->sa, b4, sizeof(b4)) : "(no A)",
            " / ",
            v6->len ? sa_str((struct sockaddr *)&v6->sa, b6, sizeof(b6)) : "(no AAAA)" };
        log_infon(parts, 6);
    }
    return 0;
}

int resolve_addr_check(const char *host, const struct sockaddr *cur) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res;
    if (getaddrinfo(host, NULL, &hints, &res) != 0)
        return -1;
    int found = 0;
    for (struct addrinfo *ai = res; ai && !found; ai = ai->ai_next)
        found = sa_addr_eq(ai->ai_addr, cur);
    freeaddrinfo(res);
    return found ? 0 : 1;
}

/* Host half of "host:port" plus a pointer to whatever follows the colon —
 * a single port for AWG_LISTEN, a whole set for AWG_REMOTE. */
static int split_host_port(const char *s, char *host, int hostmax,
                           const char **portstr) {
    const char *hstart = s, *hend = NULL, *colon;

    if (*s == '[') {
        /* [2001:db8::1]:443 — the only unambiguous IPv6 form */
        hstart = s + 1;
        for (const char *q = hstart; *q; q++)
            if (*q == ']') { hend = q; break; }
        if (!hend || hend[1] != ':') return -1;
        colon = hend + 1;
    } else {
        /* A bare IPv6 literal has no room for a port, and splitting it on the
         * last colon would silently produce a wrong host. Reject it outright. */
        struct in6_addr tmp;
        if (inet_pton(AF_INET6, s, &tmp) == 1) return -1;
        for (const char *q = s; *q; q++)
            if (*q == ':') hend = q;
        if (!hend) return -1;
        colon = hend;
    }

    int hlen = (int)(hend - hstart);
    if (hlen >= hostmax) return -1;
    memcpy(host, hstart, (size_t)hlen);
    host[hlen] = '\0';

    if (!colon[1]) return -1;
    *portstr = colon + 1;
    return 0;
}

int parse_host_port(const char *s, char *host, int hostmax, uint16_t *port) {
    const char *pstr;
    if (split_host_port(s, host, hostmax, &pstr) < 0) return -1;

    uint32_t v = 0;
    for (const char *q = pstr; *q; q++) {
        if (*q < '0' || *q > '9') return -1;
        v = v * 10 + (uint32_t)(*q - '0');
        if (v > 65535) return -1;
    }
    *port = (uint16_t)v;
    return 0;
}

/* One token: "443" or "20150-20299". A reversed range is a typo (a swapped
 * pair of digits in a config), and quietly turning it around would hide the
 * mistake instead of reporting it. */
static const char *parse_port_num(const char *q, uint32_t *out) {
    uint32_t v = 0;
    int digits = 0;
    for (; *q >= '0' && *q <= '9'; q++) {
        v = v * 10 + (uint32_t)(*q - '0');
        if (v > 65535) return NULL;
        digits++;
    }
    if (!digits || v == 0) return NULL;
    *out = v;
    return q;
}

static int parse_port_set(const char *s, portset_t *ps) {
    ps->n = 0;
    ps->total = 0;
    for (const char *q = s;;) {
        uint32_t lo, hi;
        if (!(q = parse_port_num(q, &lo))) return -1;
        hi = lo;
        if (*q == '-' && !(q = parse_port_num(q + 1, &hi))) return -1;
        if (hi < lo) return -1;
        if (ps->n >= AWG_MAX_PORT_RANGES) return -1;
        ps->r[ps->n].lo = (uint16_t)lo;
        ps->r[ps->n].hi = (uint16_t)hi;
        ps->n++;
        ps->total += hi - lo + 1;
        if (!*q) return 0;
        if (*q != ',') return -1;
        q++;
    }
}

int parse_host_ports(const char *s, char *host, int hostmax, portset_t *ps) {
    const char *pstr;
    if (split_host_port(s, host, hostmax, &pstr) < 0) return -1;
    return parse_port_set(pstr, ps);
}

/* idx-th port of the set, counting through the ranges in order. */
static uint16_t portset_at(const portset_t *ps, uint32_t idx) {
    for (uint8_t i = 0; i < ps->n; i++) {
        uint32_t sz = (uint32_t)ps->r[i].hi - ps->r[i].lo + 1;
        if (idx < sz) return (uint16_t)(ps->r[i].lo + idx);
        idx -= sz;
    }
    return ps->r[0].lo;   /* unreachable while total matches the ranges */
}

uint16_t portset_pick(const portset_t *ps, fastrand_t *rng, uint16_t avoid) {
    uint16_t port = portset_at(ps, (uint32_t)fastrand_intn(rng, (int)ps->total));
    /* A single redraw, not a loop: on a two-port set the loop would spin, and
     * landing on the dead port twice in a row only costs one more hop. */
    if (port == avoid && ps->total > 1)
        port = portset_at(ps, (uint32_t)fastrand_intn(rng, (int)ps->total));
    return port;
}

static int create_udp_socket(int family, int blocking) {
    int flags = SOCK_DGRAM | SOCK_CLOEXEC;
    if (!blocking) flags |= SOCK_NONBLOCK;
    return socket(family, flags, 0);
}

/* SO_RCVBUF is silently clamped to net.core.rmem_max, which on a router is
 * usually the 208 KiB default — about 8 ms of traffic at 200 Mbit/s. One
 * scheduling delay longer than that and the kernel drops the overflow before
 * the proxy ever sees it, invisibly: nothing in the container's own counters
 * is exported to the host. SO_RCVBUFFORCE ignores the sysctl ceiling for a
 * caller holding CAP_NET_ADMIN, which is exactly the case in a MikroTik
 * container, so try that first and keep the clamped version as the fallback
 * for unprivileged runs. */
static void set_socket_buffers(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)) < 0) {
        log_debug2("SO_RCVBUFFORCE refused: ", strerror(errno));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &size, sizeof(size)) < 0) {
        log_debug2("SO_SNDBUFFORCE refused: ", strerror(errno));
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }
}

/* IPv6 has no DF bit — a v6 router never fragments — so there the sockopt only
 * stops the local stack from honouring PMTU replies and from setting
 * IPV6_DONTFRAG on the send path. Kept symmetric so AWG_NO_DF means the same
 * thing on both families. */
static void set_df_off(int fd, int family) {
    int val = (family == AF_INET6) ? IPV6_PMTUDISC_DONT : IP_PMTUDISC_DONT;
    int level = (family == AF_INET6) ? IPPROTO_IPV6 : IPPROTO_IP;
    int opt = (family == AF_INET6) ? IPV6_MTU_DISCOVER : IP_MTU_DISCOVER;
    if (setsockopt(fd, level, opt, &val, sizeof(val)) < 0)
        log_error2("MTU_DISCOVER dont failed: ", strerror(errno));
}

/* Busy polling is what gets a packet out of the NIC ring before the thread
 * would have been woken for it, and on a router that is the difference between
 * catching a burst and losing its tail: the receive buffer is capped at
 * net.core.rmem_max and cannot be raised from inside a container.
 *
 * Three knobs, in order of how early they act:
 *   SO_BUSY_POLL        - the thread polls the NAPI itself instead of sleeping,
 *                         but only while it sits inside recv();
 *   SO_PREFER_BUSY_POLL - tells NAPI to hold back its own softirq and let the
 *                         poller drain the ring, so the two stop fighting over
 *                         the same queue (kernel 5.11+, harmless if refused);
 *   SO_BUSY_POLL_BUDGET - packets one poll pass may take. A full batch is the
 *                         useful unit: the caller drains into a BATCH_SIZE
 *                         array anyway, and a burst is what we are chasing. */
static const char *kernel_release(char *buf, size_t len) {
    int fd = open("/proc/sys/kernel/osrelease", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "?";
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) return "?";
    buf[n] = 0;
    char *nl = strchr(buf, 0x0A);
    if (nl) *nl = 0;
    return buf;
}

static void set_busy_poll(int fd, int usec) {
    if (usec <= 0) return;
    int ll_err = 0, prefer_ok = 0, budget_ok = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec)) < 0)
        ll_err = errno;
#ifdef SO_PREFER_BUSY_POLL
    int prefer = 1;
    prefer_ok = setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL,
                           &prefer, sizeof(prefer)) == 0;
#endif
#ifdef SO_BUSY_POLL_BUDGET
    int budget = BATCH_SIZE * 2;
    budget_ok = setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET,
                           &budget, sizeof(budget)) == 0;
#endif
    /* Say once what the kernel actually did with each of the three, because
     * none of them can be taken for granted here. Raising sk_ll_usec above the
     * sysctl default wants CAP_NET_ADMIN, which a RouterOS container is not
     * given - the same refusal SO_RCVBUFFORCE gets - and the other two landed
     * in 5.11, which is newer than the kernel RouterOS ships. A silent failure
     * here would look exactly like a setting that works and does nothing, and
     * that is a trap worth one log line. */
    static int reported = 0;
    if (!reported) {
        reported = 1;
        char kb[64];
        const char *parts[] = { "busy-poll: kernel ", kernel_release(kb, sizeof(kb)),
                                ", SO_BUSY_POLL=", ll_err ? strerror(ll_err) : "ok",
                                " prefer=", prefer_ok ? "yes" : "no",
                                " budget=", budget_ok ? "yes" : "no" };
        log_infon(parts, 8);
    }
}

static void set_thread_affinity(int cpu, const char *name) {
    if (cpu < 0) return;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) == 0) {
        char buf[12];
        const char *parts[] = { name, " pinned to cpu", u32_to_str(buf, cpu) };
        log_infon(parts, 3);
    }
}

static void log_socket_buffers(int fd, const awg_config_t *cfg, const char *label) {
    int r = 0, w = 0;
    socklen_t len = sizeof(r);
    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &r, &len);
    len = sizeof(w);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &w, &len);
    char rb[12], wb[12], reqb[12];
    const char *parts[] = { label, " socket buf: requested=",
        u32_to_str(reqb, cfg->socket_buf / 1024), "KB, actual read=",
        u32_to_str(rb, r / 1024), "KB write=",
        u32_to_str(wb, w / 1024), "KB" };
    log_infon(parts, 8);
}

/* Warn once per connection when the tunnel actually runs over IPv6: the 40-byte
 * IPv6 header pushes a full-size WireGuard packet past 1500 at the MTU that is
 * safe over IPv4. The proxy cannot fix this — the MTU belongs to the router's
 * wireguard interface — so it just states the ceiling. */
static void log_ipv6_mtu_hint(proxy_t *p, const char *why) {
    if (g_log_level < LOG_ERROR) return;
    if (atomic_exchange_explicit(&p->fe_mtu_hint, 1, memory_order_relaxed)) return;
    char mb[12];
    const char *parts[] = { why, ": set the WireGuard interface MTU to ",
        u32_to_str(mb, (uint32_t)awg_max_wg_mtu(p->cfg->max_s4, 1)),
        " or lower — the 40-byte IPv6 header makes a full-size packet exceed "
        "1500 bytes and it will be dropped or fragmented" };
    log_msgn("WARN: ", parts, 4);
}

/* The IPv6 hint above only fires when the transport is v6, but a large enough
 * S4 overflows 1500 on IPv4 just as well: at S4=148 even the modest MTU 1380
 * yields a 1588-byte datagram. Nothing in the router's own counters shows the
 * resulting fragmentation — it just halves throughput — so warn on the packet
 * the proxy is actually about to put on the wire. */
static void log_frag_warn(proxy_t *p, int v6, int outer) {
    if (g_log_level < LOG_ERROR) return;
    if (atomic_exchange_explicit(&p->fe_frag_warn, 1, memory_order_relaxed)) return;
    char ob[12], mb[12];
    const char *parts[] = { "outgoing packet is ", u32_to_str(ob, (uint32_t)outer),
        " bytes and will be fragmented: set the WireGuard interface MTU to ",
        u32_to_str(mb, (uint32_t)awg_max_wg_mtu(p->cfg->max_s4, v6)),
        " or lower (S4 padding is added to every data packet)" };
    log_msgn("WARN: ", parts, 5);
}

/* Open and connect one socket to a resolved endpoint. */
static int dial_one(proxy_t *p, const awg_addr_t *a, int blocking) {
    int family = a->sa.ss_family;
    int fd = create_udp_socket(family, blocking);
    if (fd < 0) return -1;

    /* The probe holds two sockets at once; without V6ONLY the v6 one may also
     * claim the v4 wildcard and collide with its sibling on the same port. */
    if (family == AF_INET6) {
        int on = 1;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    }

    if (p->local_port > 0) {
        struct sockaddr_storage local;
        socklen_t local_len;
        memset(&local, 0, sizeof(local));
        if (family == AF_INET6) {
            struct sockaddr_in6 *l6 = (struct sockaddr_in6 *)&local;
            l6->sin6_family = AF_INET6;
            l6->sin6_port = htons(p->local_port);
            local_len = sizeof(*l6);
        } else {
            struct sockaddr_in *l4 = (struct sockaddr_in *)&local;
            l4->sin_family = AF_INET;
            l4->sin_port = htons(p->local_port);
            local_len = sizeof(*l4);
        }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(fd, (struct sockaddr *)&local, local_len) < 0) {
            log_error2("bind failed: ", strerror(errno));
            close(fd);
            return -1;
        }
    }

    if (connect(fd, (struct sockaddr *)&a->sa, a->len) < 0) {
        log_error2("connect failed: ", strerror(errno));
        close(fd);
        return -1;
    }

    if (g_log_level >= LOG_INFO) {
        char ipbuf[INET6_ADDRSTRLEN], pbuf[12];
        const char *parts[] = { "connected to ",
            sa_str((struct sockaddr *)&a->sa, ipbuf, sizeof(ipbuf)), " port ",
            u32_to_str(pbuf, p->remote_port) };
        log_infon(parts, 4);
    }

    set_socket_buffers(fd, p->cfg->socket_buf);
    set_busy_poll(fd, p->cfg->busy_poll);
    if (p->cfg->no_df)
        set_df_off(fd, family);
    return fd;
}

/* ---- Learned transport preference ---- */

int state_read_prefer6(const char *path) {
    if (!path || !path[0]) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char c = 0;
    ssize_t n = read(fd, &c, 1);
    close(fd);
    return n == 1 && c == '6';
}

int state_write_prefer6(const char *path, int prefer6) {
    if (!path || !path[0]) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    char c = prefer6 ? '6' : '4';
    ssize_t n = write(fd, &c, 1);
    close(fd);
    return n == 1 ? 0 : -1;
}

/* Record which family actually carried traffic so the next start dials it
 * first instead of paying the head start on a dead one. Writes at most once
 * per run, and only when the byte on disk is stale: the router's flash is
 * small and a flapping link must never turn into a write loop. A write that
 * fails (read-only root, say) still counts as done — the in-memory preference
 * keeps steering this run, and retrying every reconnect would be the write
 * loop this guards against.
 *
 * Note what the two values mean. '6' is an optimisation — skip the doomed IPv4
 * head start. '4' is NOT "IPv6 is disabled": it only restores the stock order,
 * in which the IPv6 socket is still opened and still probed on every connect
 * and reconnect. So an IPv6 outage is never learned permanently — the moment
 * IPv4 goes silent again, IPv6 gets another chance and can win back.
 *
 * prefer6/state_written are plain ints on purpose, unlike the atomics around
 * them: proxy_init() sets them before any thread exists, and from then on only
 * the s2c thread touches them — it owns he_probe()/he_finish() and is the sole
 * caller of do_reconnect(), hence of dial_remote(). */
static void he_learn(proxy_t *p, int prefer6) {
    if (prefer6 == p->prefer6) return;
    /* The in-memory preference must keep following the latest verdict, even
     * after the one allowed flash write. Gating it on state_written froze the
     * dial order for the whole run: a peer that moved to the other family (a
     * CGNAT IPv4 that stopped accepting inbound while its AAAA stayed valid,
     * say) had every later reconnect start on the dead family again and lean
     * on the probe to rescue it. Only the disk write stays once-per-run. */
    p->prefer6 = prefer6;
    if (p->state_written) return;
    p->state_written = 1;
    if (state_write_prefer6(p->cfg->state_file, prefer6) == 0)
        log_info(prefer6 ? "remembered: dial IPv6 first from now on"
                         : "remembered: dial IPv4 first from now on");
}

/* Resolve and connect. When the name carries both an A and an AAAA record the
 * second socket is opened too and left for he_probe() to arbitrate; the primary
 * (returned) socket is the IPv4 one, matching what every previous release did —
 * IPv6 only takes over once it demonstrably answers. The one exception is a
 * preference learned by an earlier run, which flips the order so a site whose
 * IPv4 is dead stops paying the probe delay on every single reconnect. */
static int dial_remote(proxy_t *p, int blocking) {
    /* A fresh port for every dial. Both callers — the startup dial, before the
     * threads exist, and do_reconnect() from the s2c thread — are serialised
     * by construction, so rng_s2c needs no locking, and every reconnect there
     * is (timeout, socket error, a new IP from DNS, a fallback stage, the
     * rebind of AWG_SRC_PORT=auto) moves off the port that just failed. With a
     * single-port AWG_REMOTE this is a no-op. */
    p->remote_port = portset_pick(&p->remote_ports, &p->rng_s2c, p->remote_port);

    awg_addr_t v4, v6;
    if (resolve_addr(p->remote_host, p->remote_port, &v4, &v6) < 0)
        return -1;

    /* A long outage invalidates whatever an earlier probe concluded: the family
     * that used to answer may be exactly the one that died. The watchdog only
     * raises the flag; consuming it here keeps prefer6 written by the s2c thread
     * alone. Back to the neutral order, and let the probe decide again — the
     * flash copy is left alone, it is written at most once per run. */
    if (atomic_exchange_explicit(&p->he_reset, 0, memory_order_relaxed)) {
        p->prefer6 = 0;
        log_info("connection lost for too long: trying IPv4 and IPv6 afresh");
    }

    const awg_addr_t *primary, *alt;
    if (p->prefer6 && v6.len) {
        primary = &v6;
        alt     = v4.len ? &v4 : NULL;
    } else {
        primary = v4.len ? &v4 : &v6;
        alt     = v4.len ? (v6.len ? &v6 : NULL) : NULL;
    }

    int fd = dial_one(p, primary, blocking);
    if (fd < 0) {
        /* A dead route on one family must not hide a working other one. */
        if (!alt) return -1;
        fd = dial_one(p, alt, blocking);
        if (fd < 0) return -1;
        primary = alt;
        alt = NULL;
        /* connect() on UDP fails when there is no route, so this is a real
         * verdict on the family, not just a slow path. */
        he_learn(p, primary->sa.ss_family == AF_INET6);
    }
    p->remote = *primary;

    atomic_store_explicit(&p->he_sent, 0, memory_order_relaxed);
    p->he_pkt_len = 0;
    p->remote_alt.len = 0;

    int fd2 = -1;
    if (alt) {
        fd2 = dial_one(p, alt, blocking);
        if (fd2 >= 0)
            p->remote_alt = *alt;
    }
    atomic_store_explicit(&p->remote_fd2, fd2, memory_order_release);

    if (fd2 < 0 && p->remote.sa.ss_family == AF_INET6)
        log_ipv6_mtu_hint(p, "remote is IPv6");
    return fd;
}

/* ---- Happy Eyeballs (RFC 8305), adapted to UDP ---- */

/* One probe window. Three WireGuard handshake retries (5 s apart) fit inside
 * it, so a family that can answer at all has had several chances before the
 * window closes. Closing it does not end the probe when both stayed silent —
 * it only slows the replay down (see he_probe). */
#define HE_PROBE_MAX_MS 15000u
/* Ceiling for that slow-down. A path that answers nothing then costs one
 * datagram every few seconds, the same order as WireGuard's own retry. */
#define HE_PROBE_SLOW_MS 5000

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t mono_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ---- Spin-drain ---------------------------------------------------------
 *
 * SO_BUSY_POLL is the kernel's own answer to "take the packet before the thread
 * would have been woken for it", and inside a RouterOS container it is not
 * available: raising sk_ll_usec past the sysctl default needs CAP_NET_ADMIN,
 * which the router does not hand out - the same refusal SO_RCVBUFFORCE gets.
 *
 * What is left is to not fall asleep in the first place. When a read comes up
 * empty, keep retrying without blocking for a short budget before letting the
 * thread be parked: a burst arriving inside that budget is picked up with no
 * wakeup in the path at all, and anything slower falls through to an ordinary
 * blocking read, so an idle tunnel still costs nothing.
 *
 * Both helpers end on a blocking call and never return EAGAIN - their callers
 * read a negative return as "go round again", and would spin a core flat. */
/* A retry costs a syscall, and a syscall on a UDP socket takes the same lock
 * the softirq needs to put the next packet in. Hammering it flat out fights
 * the delivery it is waiting for, so back off a hair between attempts: a few
 * hundred nanoseconds of yield keeps the thread on its core and awake while
 * leaving the lock alone. */
#define SPIN_PAUSE_SPINS 64

static inline void spin_pause(void) {
    for (int i = 0; i < SPIN_PAUSE_SPINS; i++) {
        /* YIELD is ARMv6K and up; on ARMv5 (the armv5 build target) the
         * assembler rejects it outright, so the hint has to be version-gated
         * rather than architecture-gated. Where there is no hint instruction
         * the compiler barrier alone still keeps the loop from being hoisted. */
#if defined(__aarch64__) || (defined(__ARM_ARCH) && __ARM_ARCH >= 7)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#else
        __asm__ __volatile__("" ::: "memory");
#endif
    }
}

static int spin_recvmsg(proxy_t *p, int fd, struct msghdr *h) {
    int spin = atomic_load_explicit(&p->spin_us, memory_order_relaxed);
    size_t clen = h->msg_controllen;
    if (spin > 0) {
        uint64_t deadline = mono_us() + (uint64_t)spin;
        do {
            ssize_t n = recvmsg(fd, h, MSG_DONTWAIT);
            if (n >= 0) return (int)n;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return (int)n;
            h->msg_controllen = clen;
            h->msg_flags = 0;
            spin_pause();
        } while (mono_us() < deadline);
    }
    h->msg_controllen = clen;
    h->msg_flags = 0;
    return (int)recvmsg(fd, h, 0);
}

static int spin_recvmmsg(proxy_t *p, int fd, struct mmsghdr *msgs, int vlen) {
    int spin = atomic_load_explicit(&p->spin_us, memory_order_relaxed);
    if (spin > 0) {
        uint64_t deadline = mono_us() + (uint64_t)spin;
        do {
            int n = recvmmsg(fd, msgs, vlen, MSG_DONTWAIT, NULL);
            if (n > 0) return n;
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return n;
            spin_pause();
        } while (mono_us() < deadline);
    }
    return recvmmsg(fd, msgs, vlen, MSG_WAITFORONE, NULL);
}

/* c2s: keep a copy of the first packet sent after a (re)connect so the probe
 * has something to replay. Cheap enough for the hot path — the probe socket is
 * -1 in every single-family config, so this is one predicted branch on a
 * relaxed load. A handshake init passes replace=1: it is the only packet a
 * server ever answers, so it displaces whatever transport frame happened to go
 * out first and restarts the clock. */
static inline void he_stash(proxy_t *p, const void *data, int len, int replace) {
    if (atomic_load_explicit(&p->remote_fd2, memory_order_relaxed) < 0) return;
    if (!replace && atomic_load_explicit(&p->he_sent, memory_order_relaxed)) return;
    if (len <= 0 || len > (int)sizeof(p->he_pkt)) return;
    atomic_store_explicit(&p->he_sent, 0, memory_order_relaxed);
    memcpy(p->he_pkt, data, (size_t)len);
    p->he_pkt_len = len;
    p->he_sent_ms = mono_ms();
    atomic_store_explicit(&p->he_sent, 1, memory_order_release);
    if (p->he_evfd >= 0) {
        uint64_t one = 1;
        (void)!write(p->he_evfd, &one, sizeof(one));
    }
}

/* Settle on one family and drop the loser. Runs in the s2c thread before it
 * starts reading, so remote_fd is swapped while nothing is mid-recv on it. */
static void he_finish(proxy_t *p, int use_alt, int learn) {
    int fd2 = atomic_load_explicit(&p->remote_fd2, memory_order_acquire);
    if (fd2 < 0) return;
    atomic_store_explicit(&p->remote_fd2, -1, memory_order_release);

    if (use_alt) {
        int old = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
        p->remote = p->remote_alt;
        atomic_store_explicit(&p->remote_fd, fd2, memory_order_release);
        if (old >= 0) close(old);
        log_info(p->remote.sa.ss_family == AF_INET6
                     ? "happy eyeballs: IPv6 answered first, using it"
                     : "happy eyeballs: IPv4 answered first, using it");
    } else {
        close(fd2);
    }
    p->remote_alt.len = 0;

    if (p->remote.sa.ss_family == AF_INET6)
        log_ipv6_mtu_hint(p, "remote is IPv6");

    /* Both families were on the table and one of them demonstrably answered.
     * A probe that merely timed out passes learn=0: nothing answered, so there
     * is no verdict to remember — switching families there is a retry, not a
     * measurement. */
    if (learn)
        he_learn(p, p->remote.sa.ss_family == AF_INET6);
}

/* UDP has no handshake, so the only liveness signal is a packet coming back.
 * Both sockets are watched with poll(), which merely peeks — the winning
 * datagram stays queued for the normal read path. IPv4 gets a head start of
 * he_delay ms; after that the first outbound packet is replayed over IPv6.
 * Replaying is safe: both copies carry the same TAI64N, so a server that
 * received both rejects the second as a replay and answers exactly once. */
static void he_probe(proxy_t *p) {
    int fd  = atomic_load_explicit(&p->remote_fd,  memory_order_acquire);
    int fd2 = atomic_load_explicit(&p->remote_fd2, memory_order_acquire);
    if (fd < 0 || fd2 < 0) return;

    struct pollfd pfd[3] = {
        { fd, POLLIN, 0 }, { fd2, POLLIN, 0 }, { p->he_evfd, POLLIN, 0 }
    };
    nfds_t nfds = (p->he_evfd >= 0) ? 3 : 2;
    int use_alt = 0, dup_logged = 0, answered = 0;

    /* The replay is a single UDP datagram, so it can be lost like any other.
     * Replaying only once meant one drop cost the whole probe: the alt family
     * was never sounded again and the run stayed pinned to a primary that had
     * stopped answering until the 180 s silence watchdog forced a reconnect —
     * which dialled the same dead primary first and repeated the cycle. Keep
     * re-sending every he_delay, and give the whole probe a deadline. */
    uint64_t probe_start = mono_ms();
    /* The first replay honours he_delay exactly, including 0 — "duplicate the
     * first packet immediately" is what the knob documents. The repeats need a
     * floor, because he_delay=0 would otherwise spin the loop resending as fast
     * as poll() returns. */
    const int dup_first = p->cfg->he_delay > 0 ? p->cfg->he_delay : 0;
    int dup_every = p->cfg->he_delay > 0 ? p->cfg->he_delay : 250;
    uint64_t last_dup_ms = 0;
    int replayed = 0;
    int quiet_logged = 0;

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed) &&
           !atomic_load_explicit(&p->reconnect_needed, memory_order_relaxed)) {
        uint64_t now = mono_ms();
        if (now - probe_start >= HE_PROBE_MAX_MS) {
            /* Nothing was stashed, so the client has sent nothing at all — an
             * idle tunnel right after a reconnect looks exactly like this. With
             * no packet to replay there is nothing to probe with, and holding
             * the second socket open buys nothing. */
            if (!atomic_load_explicit(&p->he_sent, memory_order_acquire)) {
                log_info("happy eyeballs: nothing to probe with, keeping current");
                he_finish(p, 0, 0);
                return;
            }
            /* Both families stayed mute for the whole window. That is not a
             * verdict — silence never is — so the primary stays and the dial
             * order learns nothing. But closing the alt socket here would also
             * make the run deaf: whichever family comes back first, nothing
             * would notice until the silence watchdog forced a reconnect a
             * whole timeout later. That is the gap that made recovery take the
             * best part of a minute after an outage.
             *
             * So keep both sockets and keep sounding them, just slower. The
             * replay interval doubles up to HE_PROBE_SLOW_MS, which turns a
             * dead address from four packets a second into one every few
             * seconds, and the moment either family answers the probe commits
             * to it. The loop still ends on shutdown or on the watchdog's
             * reconnect, so it cannot run away. */
            if (!quiet_logged) {
                quiet_logged = 1;
                log_info("happy eyeballs: both silent, keeping current, "
                         "still listening on both");
            }
            probe_start = now;
            if (dup_every < HE_PROBE_SLOW_MS) {
                dup_every *= 2;
                if (dup_every > HE_PROBE_SLOW_MS) dup_every = HE_PROBE_SLOW_MS;
            }
            continue;
        }

        /* Without the eventfd there is nothing to wake us when c2s sends, so
         * fall back to re-checking every he_delay. */
        int wait = (nfds == 3) ? 1000 : dup_every;
        if (atomic_load_explicit(&p->he_sent, memory_order_acquire)) {
            /* First replay is due he_delay after the stash, every repeat
             * dup_every after the previous replay. */
            uint64_t since = replayed ? (now - last_dup_ms)
                                      : (now - p->he_sent_ms);
            int64_t left = (int64_t)(replayed ? dup_every : dup_first) -
                           (int64_t)since;
            if (left < wait) wait = left > 0 ? (int)left : 0;
        }
        {   /* Never sleep past the probe deadline. */
            int64_t to_deadline = (int64_t)HE_PROBE_MAX_MS -
                                  (int64_t)(now - probe_start);
            if (to_deadline < wait) wait = to_deadline > 0 ? (int)to_deadline : 0;
        }

        int r = poll(pfd, nfds, wait);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r > 0 && pfd[2].revents) {
            /* c2s just stashed a packet — drain and recompute the deadline
             * from its timestamp rather than treating this as a winner. */
            uint64_t v;
            (void)!read(p->he_evfd, &v, sizeof(v));
            pfd[2].revents = 0;
            r--;
        }
        if (r > 0) {
            /* A readable socket wins outright; a bare POLLERR (ICMP
             * unreachable) only proves the other family should be preferred. */
            if (pfd[0].revents & POLLIN)      use_alt = 0;
            else if (pfd[1].revents & POLLIN) use_alt = 1;
            else                              use_alt = (pfd[0].revents != 0);
            answered = 1;
            break;
        }
        if (atomic_load_explicit(&p->he_sent, memory_order_acquire) &&
            p->he_pkt_len > 0) {
            now = mono_ms();
            uint64_t since = replayed ? (now - last_dup_ms)
                                      : (now - p->he_sent_ms);
            if ((int64_t)since >= (int64_t)(replayed ? dup_every : dup_first)) {
                send(fd2, p->he_pkt, (size_t)p->he_pkt_len,
                     MSG_DONTWAIT | MSG_NOSIGNAL);
                last_dup_ms = now;
                replayed = 1;
                /* One line per probe, not one per retry — the retries are a
                 * loop now and the log lives in the router's RAM. */
                if (!dup_logged) {
                    dup_logged = 1;
                    log_info(p->remote.sa.ss_family == AF_INET6
                                 ? "happy eyeballs: IPv6 silent, probing IPv4"
                                 : "happy eyeballs: IPv4 silent, probing IPv6");
                }
            }
        }
    }

    /* Only a socket that actually spoke (or reported an ICMP error, which is
     * evidence too) is a verdict. A loop cut short by shutdown or reconnect
     * measured nothing, so it must not teach the dial order anything. */
    he_finish(p, use_alt, answered);
}

/* ---- GRO/GSO ---- */

static int enable_gro(int fd) {
    int val = 1;
    return setsockopt(fd, IPPROTO_UDP, UDP_GRO, &val, sizeof(val)) == 0;
}

/* Reads to wait through before offering GRO another chance, doubling on each
 * refusal up to the cap. The give-up rule below counts single-segment reads,
 * which an idle tunnel produces by the dozen — one keepalive every 25 s is
 * enough to retire GRO for the whole session. Coalescing matters most under
 * load: without it the socket is drained one datagram per syscall, and the
 * receive queue overflows well before the CPU runs out. */
#define GRO_REARM_MIN   512
#define GRO_REARM_MAX   32768

/* Giving up on GRO has to clear the sockopt too: the kernel keeps coalescing
 * while UDP_GRO is set, and the recvmmsg fallback has no way to split a merged
 * buffer, so it would forward several packets glued into one. */
static void disable_gro(int fd) {
    int val = 0;
    setsockopt(fd, IPPROTO_UDP, UDP_GRO, &val, sizeof(val));
}

/* Segment size of a coalesced read, 0 when the kernel did not coalesce.
 * On receive the kernel reports it as UDP_GRO holding an int
 * (udp_cmsg_recv()); UDP_SEGMENT is the send-side GSO type and never appears
 * here. */
int gro_seg_size(const struct msghdr *hdr) {
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(hdr); cm; cm = CMSG_NXTHDR((struct msghdr *)hdr, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_GRO) {
            int ss;
            memcpy(&ss, CMSG_DATA(cm), sizeof(ss));
            return ss;
        }
    }
    return 0;
}

static void init_gro_state(proxy_t *p) {
    /* s2c GRO (remote → client) */
    p->gro_iov.iov_base = p->gro_buf;
    p->gro_iov.iov_len = GRO_BUF_SIZE;
    memset(&p->gro_hdr, 0, sizeof(p->gro_hdr));
    p->gro_hdr.msg_iov = &p->gro_iov;
    p->gro_hdr.msg_iovlen = 1;
    p->gro_hdr.msg_control = p->gro_cmsg;
    p->gro_hdr.msg_controllen = sizeof(p->gro_cmsg);

    /* c2s GRO (client → remote) */
    p->gro_iov_c2s.iov_base = p->gro_buf_c2s;
    p->gro_iov_c2s.iov_len = GRO_BUF_SIZE;
    memset(&p->gro_hdr_c2s, 0, sizeof(p->gro_hdr_c2s));
    p->gro_hdr_c2s.msg_iov = &p->gro_iov_c2s;
    p->gro_hdr_c2s.msg_iovlen = 1;
    p->gro_hdr_c2s.msg_control = p->gro_cmsg_c2s;
    p->gro_hdr_c2s.msg_controllen = sizeof(p->gro_cmsg_c2s);
    p->gro_hdr_c2s.msg_name = &p->gro_addr_c2s;
    p->gro_hdr_c2s.msg_namelen = p->cli_len;
}

/* recv_gro: blocking recvmsg with GRO. Returns total bytes, sets *seg_size.
 * seg_size=0 means no coalescing (single packet). */
static int recv_gro(proxy_t *p, int fd, int *seg_size) {
    p->gro_hdr.msg_controllen = sizeof(p->gro_cmsg);
    p->gro_hdr.msg_flags = 0;

    ssize_t n = spin_recvmsg(p, fd, &p->gro_hdr);
    if (n <= 0) {
        *seg_size = 0;
        return (int)n;
    }

    *seg_size = gro_seg_size(&p->gro_hdr);

    return (int)n;
}

/* send_gso: send a prefix of same-size packets via one sendmsg with UDP_SEGMENT.
 * Returns number of packets sent, or negative errno on error. */
static int send_gso(int fd, struct iovec *iovecs, int count,
                    cliaddr_t *addr) {
    if (count <= 1) return 0;

    /* Find longest prefix of same-size packets */
    int seg_size = (int)iovecs[0].iov_len;
    int gso_count = 1;
    while (gso_count < count && (int)iovecs[gso_count].iov_len == seg_size)
        gso_count++;
    /* Last segment may be shorter per GSO spec */
    if (gso_count < count && (int)iovecs[gso_count].iov_len < seg_size)
        gso_count++;
    if (gso_count <= 1) return 0;

    /* Build cmsg with UDP_SEGMENT */
    union {
        char buf[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } cmsg_u;
    memset(&cmsg_u, 0, sizeof(cmsg_u));

    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = iovecs;
    hdr.msg_iovlen = gso_count;
    hdr.msg_control = cmsg_u.buf;
    hdr.msg_controllen = sizeof(cmsg_u.buf);

    struct cmsghdr *cm = CMSG_FIRSTHDR(&hdr);
    cm->cmsg_level = IPPROTO_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t ss = (uint16_t)seg_size;
    memcpy(CMSG_DATA(cm), &ss, sizeof(ss));

    if (addr) {
        hdr.msg_name = addr;
        hdr.msg_namelen = cliaddr_len(addr);
    }

    ssize_t ret = sendmsg(fd, &hdr, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (ret < 0) return -errno;
    return gso_count;
}

/* ---- Init ---- */

int proxy_init(proxy_t *p, awg_config_t *cfg,
               const char *listen_str, const char *remote_str, int src_port) {
    const char *cfg_err = NULL;
    memset(p, 0, sizeof(*p));
    p->cfg = cfg;
    p->listen_fd = -1;
    atomic_store_explicit(&p->remote_fd, -1, memory_order_relaxed);
    atomic_store_explicit(&p->remote_fd2, -1, memory_order_relaxed);
    /* Only ever used by the Happy Eyeballs probe; a failure here just costs
     * timing precision, so it is not fatal. */
    p->he_evfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    p->signal_fd = -1;
    p->timer_fd = -1;
    p->gso_ok = 1;

    if (config_validate(cfg, &cfg_err) < 0) {
        log_error2("invalid config: ", cfg_err);
        return -1;
    }

    /* Which family an earlier run settled on, if any (see he_learn). */
    p->prefer6 = state_read_prefer6(cfg->state_file);
    if (p->prefer6)
        log_info("learned preference: dialing IPv6 first");

    /* Parse listen address */
    char host[256];
    uint16_t port;
    if (parse_host_port(listen_str, host, sizeof(host), &port) < 0)
        return -1;
    /* Family of the client-facing leg. IPv4 unless AWG_LISTEN names an IPv6
     * host: ":51820" and "0.0.0.0:51820" keep the socket exactly as it always
     * was, "[::]:51820" opens it to both families, "[2a00:..]:51820" pins it
     * to one address. parse_host_port has already stripped the brackets. */
    memset(&p->listen_addr, 0, sizeof(p->listen_addr));
    if (host[0] && inet_pton(AF_INET6, host, &p->listen_addr.v6.sin6_addr) == 1) {
        p->listen_family = AF_INET6;
        p->listen_addr.v6.sin6_family = AF_INET6;
        p->listen_addr.v6.sin6_port = htons(port);
    } else {
        p->listen_family = AF_INET;
        p->listen_addr.v4.sin_family = AF_INET;
        p->listen_addr.v4.sin_port = htons(port);
        if (host[0] && inet_pton(AF_INET, host, &p->listen_addr.v4.sin_addr) != 1)
            p->listen_addr.v4.sin_addr.s_addr = INADDR_ANY;
    }
    p->cli_len = cliaddr_len(&p->listen_addr);

    /* Parse remote address. The port half may be a set — dial_remote() draws
     * from it on every connect; until then the first port stands. */
    if (parse_host_ports(remote_str, p->remote_host, sizeof(p->remote_host),
                         &p->remote_ports) < 0) {
        log_error("AWG_REMOTE: bad host or port list");
        return -1;
    }
    p->remote_port = p->remote_ports.r[0].lo;
    if (p->remote_ports.total > 1) {
        char nb[12];
        const char *parts[] = { "remote port is picked per connection out of ",
                                u32_to_str(nb, p->remote_ports.total), " ports" };
        log_infon(parts, 3);
    }

    if (src_port > 0) {
        p->local_port = src_port;
    } else if (src_port == 0) {
        p->auto_src_port = 1;
    }
    /* src_port < 0 ("random", the default): local_port stays 0, no bind — the
     * kernel picks a fresh ephemeral port on every connect. Note that reverse
     * and server modes never reach the auto_src_port branch below anyway: it
     * lives in c2s_thread_normal(), so for them local_port has always been 0. */

    /* Init PRNG. fastrand still picks H values and junk sizes; what goes on the
     * wire byte for byte comes from csprng_bytes(). The old seeding read
     * /dev/urandom without checking the result and fell back to the address of
     * a static struct in a static non-PIE binary — a constant, so a container
     * without /dev/urandom replayed the same stream after every restart. */
    uint64_t seed;
    csprng_bytes((uint8_t *)&seed, sizeof(seed));
    fastrand_init(&p->rng, seed);
    /* The two directions must never emit the same values at the same time, so
     * they run independent streams from independent seeds. */
    fastrand_init(&p->rng_c2s, seed ^ 0x9E3779B97F4A7C15ULL);
    fastrand_init(&p->rng_s2c, seed ^ 0xBF58476D1CE4E5B9ULL);

    /* Pre-allocate junk buffers */
    if (cfg->jc > 0 && cfg->jmax > 0) {
        size_t junk_bytes;
        size_t junk_sizes_bytes;
        if (junk_layout_sizes(cfg, &junk_bytes, &junk_sizes_bytes) < 0)
            return -1;
        p->junk_buf = (uint8_t *)malloc(junk_bytes);
        p->junk_sizes = (int *)malloc(junk_sizes_bytes);
        if (!p->junk_buf || !p->junk_sizes) return -1;
    }

    /* Init H4 rings */
    fill_h4_rings(p);

    /* Init batch I/O structures — invariant fields set once.
     * Headroom is sized from the largest S4 in the fallback chain, so a
     * profile switch never changes the buffer layout: transform_outbound
     * returns buf + headroom - s4 and simply leaves the surplus unused. */
    int c2s_headroom = (cfg->mode == AWG_MODE_NORMAL) ? cfg->max_s4 : 0;
    int s2c_headroom = (cfg->mode == AWG_MODE_NORMAL) ? 0 : cfg->max_s4;
    p->c2s_headroom = c2s_headroom;
    p->s2c_headroom = s2c_headroom;
    for (int i = 0; i < BATCH_SIZE; i++) {
        /* recv_c2s: listen socket */
        p->recv_c2s.iovecs[i].iov_base = p->recv_c2s.bufs[i] + c2s_headroom;
        p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
        p->recv_c2s.msgs[i].msg_hdr.msg_iov = &p->recv_c2s.iovecs[i];
        p->recv_c2s.msgs[i].msg_hdr.msg_iovlen = 1;
    }
    /* In reverse/server mode, capture addr from every packet for routing */
    if (cfg->mode != AWG_MODE_NORMAL) {
        for (int i = 0; i < BATCH_SIZE; i++) {
            p->recv_c2s.msgs[i].msg_hdr.msg_name = &p->recv_c2s.addrs[i];
            p->recv_c2s.msgs[i].msg_hdr.msg_namelen = p->cli_len;
        }
    } else {
        /* Normal: capture client addr only from first packet in batch */
        p->recv_c2s.msgs[0].msg_hdr.msg_name = &p->recv_c2s.addrs[0];
        p->recv_c2s.msgs[0].msg_hdr.msg_namelen = p->cli_len;
    }

    for (int i = 0; i < BATCH_SIZE; i++) {
        /* send_s2c: to listen socket with client addr */
        p->send_s2c.msgs[i].msg_hdr.msg_iov = &p->send_s2c.iovecs[i];
        p->send_s2c.msgs[i].msg_hdr.msg_iovlen = 1;
        p->send_s2c.msgs[i].msg_hdr.msg_name = &p->send_s2c.addrs[i];
        p->send_s2c.msgs[i].msg_hdr.msg_namelen = p->cli_len;

        /* send_c2s: to remote, connected — no addr needed */
        p->send_c2s.msgs[i].msg_hdr.msg_iov = &p->send_c2s.iovecs[i];
        p->send_c2s.msgs[i].msg_hdr.msg_iovlen = 1;
    }

    /* recv_s2c: remote socket, connected — no addr needed */
    for (int i = 0; i < BATCH_SIZE; i++) {
        p->recv_s2c.iovecs[i].iov_base = p->recv_s2c.bufs[i] + s2c_headroom;
        p->recv_s2c.iovecs[i].iov_len = BUF_SIZE + AWG_PACKET_HEADROOM - s2c_headroom;
        p->recv_s2c.msgs[i].msg_hdr.msg_iov = &p->recv_s2c.iovecs[i];
        p->recv_s2c.msgs[i].msg_hdr.msg_iovlen = 1;
    }

    /* S4 padding is generated fresh for every packet in the send paths. A
     * pre-filled pool would give a DPI only BATCH_SIZE distinct prefixes for
     * the container's whole lifetime, and under v3 the first 12 bytes are the
     * ChaCha20 nonce — reuse there is a far stronger signal than the one it
     * would save. Two xorshift64 calls per packet is ~20 cycles. */

    /* Init GRO state */
    init_gro_state(p);

    return 0;
}

/* ---- Stats ---- */

/* One relaxed add per batch, so the accounting costs nothing measurable even
 * with stats switched off. nsend - sent is what the proxy itself threw away:
 * send_batch_gso stops at the first sendmmsg failure and abandons the tail. */
static inline void stats_add_tx(_Atomic uint32_t *tx, _Atomic uint32_t *drop,
                                int nsend, int sent) {
    if (sent > 0)
        atomic_fetch_add_explicit(tx, (uint32_t)sent, memory_order_relaxed);
    if (nsend > sent)
        atomic_fetch_add_explicit(drop, (uint32_t)(nsend - sent),
                                  memory_order_relaxed);
}

static inline void stats_add_rx(_Atomic uint32_t *rx, int n) {
    if (n > 0)
        atomic_fetch_add_explicit(rx, (uint32_t)n, memory_order_relaxed);
}

/* The kernel's own UDP drop counters for this network namespace. In a MikroTik
 * container these are invisible from RouterOS, so without reading them here
 * there is no way to tell a receive-queue overflow from a send failure. */
typedef struct {
    unsigned long long in_errors;
    unsigned long long rcvbuf_errors;
    unsigned long long sndbuf_errors;
} udp_kstats_t;

static int read_udp_kstats(udp_kstats_t *out) {
    int fd = open("/proc/net/snmp", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    /* Two "Udp:" lines: names then values, in the same column order. */
    char *names = strstr(buf, "Udp:");
    if (!names) return -1;
    char *vals = strstr(names + 4, "Udp:");
    if (!vals) return -1;
    char *names_end = strchr(names, 0x0A);
    if (!names_end || names_end > vals) return -1;

    out->in_errors = out->rcvbuf_errors = out->sndbuf_errors = 0;
    char *np = names + 4, *vp = vals + 4;
    while (np < names_end) {
        while (np < names_end && *np == ' ') np++;
        while (*vp == ' ') vp++;
        if (np >= names_end || *vp == 0 || *vp == 0x0A) break;
        unsigned long long v = 0;
        char *vstart = vp;
        while (*vp >= '0' && *vp <= '9') { v = v * 10 + (unsigned)(*vp - '0'); vp++; }
        if (vp == vstart) break;
        if (!strncmp(np, "InErrors", 8))          out->in_errors = v;
        else if (!strncmp(np, "RcvbufErrors", 12)) out->rcvbuf_errors = v;
        else if (!strncmp(np, "SndbufErrors", 12)) out->sndbuf_errors = v;
        while (np < names_end && *np != ' ') np++;
    }
    return 0;
}

/* Per-socket receive-queue drops, straight from /proc/net/udp[6].
 *
 * The netns-wide UdpRcvbufErrors says that something overflowed but not what:
 * during an upload the listen socket carries the data and the remote socket
 * only carries ACKs, and the two lead to opposite conclusions. The kernel
 * exports a per-socket counter in the last column of /proc/net/udp, keyed by
 * the local port, so read it for exactly the two sockets this proxy owns. */
static unsigned long long udp_socket_drops(int fd) {
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &slen) < 0) return 0;
    int port = ntohs(ss.ss_family == AF_INET6
                     ? ((struct sockaddr_in6 *)&ss)->sin6_port
                     : ((struct sockaddr_in *)&ss)->sin_port);
    if (port <= 0) return 0;

    /* The local address field ends with ":PORT" in uppercase hex. */
    static const char hex[] = "0123456789ABCDEF";
    char want[6];
    want[0] = ':';
    want[1] = hex[(port >> 12) & 0xF];
    want[2] = hex[(port >> 8) & 0xF];
    want[3] = hex[(port >> 4) & 0xF];
    want[4] = hex[port & 0xF];
    want[5] = 0;

    unsigned long long total = 0;
    const char *paths[2] = { "/proc/net/udp", "/proc/net/udp6" };
    for (int f = 0; f < 2; f++) {
        int fdp = open(paths[f], O_RDONLY | O_CLOEXEC);
        if (fdp < 0) continue;
        /* 64 KiB holds far more sockets than a container ever has; a truncated
         * read only costs the tail lines, never correctness. */
        static char buf[65536];
        ssize_t n = read(fdp, buf, sizeof(buf) - 1);
        close(fdp);
        if (n <= 0) continue;
        buf[n] = 0;

        for (char *line = buf; line && *line; ) {
            char *end = strchr(line, 0x0A);
            if (end) *end = 0;

            /* Walk the whitespace-separated fields: [1] is local_address,
             * the last one is the drop counter. */
            char *fields[16];
            int nf = 0;
            for (char *q = line; *q && nf < 16; ) {
                while (*q == ' ') q++;
                if (!*q) break;
                fields[nf++] = q;
                while (*q && *q != ' ') q++;
                if (*q) *q++ = 0;
            }
            if (nf >= 3) {
                char *local = fields[1];
                int llen = (int)strlen(local);
                if (llen > 5 && !strcmp(local + llen - 5, want)) {
                    unsigned long long v = 0;
                    for (char *c = fields[nf - 1]; *c >= '0' && *c <= '9'; c++)
                        v = v * 10 + (unsigned)(*c - '0');
                    total += v;
                }
            }
            line = end ? end + 1 : NULL;
        }
    }
    return total;
}
/* ---- Send helpers ---- */

/* A connected UDP socket reports a dead path synchronously, so the send itself
 * already knows what the silence watchdog would take 180 s to infer. Two cases
 * matter on a router: the source address the socket was bound to disappeared
 * (EADDRNOTAVAIL — a DHCPv6 lease that renewed into a different address, which
 * on this link expires every few minutes), and the route to the peer went away
 * (ENETUNREACH/EHOSTUNREACH/ENETDOWN). Both are permanent for this socket: no
 * later send on it can succeed, so every packet until the watchdog fires is
 * dropped on the floor. ECONNREFUSED/EPERM arrive from ICMP and mean the same
 * for a peer that is gone. EAGAIN/ENOBUFS/EMSGSIZE are deliberately absent —
 * they are per-packet and the socket stays usable. */
static int send_err_is_fatal(int err) {
    switch (err) {
    case EADDRNOTAVAIL:
    case ENETUNREACH:
    case EHOSTUNREACH:
    case ENETDOWN:
    case ECONNREFUSED:
    case EPERM:
        return 1;
    default:
        return 0;
    }
}

/* Ask for a reconnect once per broken socket. shutdown() is what makes it
 * prompt: the s2c thread is parked in a blocking recv on that fd and would not
 * look at reconnect_needed until something woke it. */
static void note_remote_send_err(proxy_t *p, int err) {
    if (!send_err_is_fatal(err)) return;
    if (atomic_exchange_explicit(&p->reconnect_needed, 1, memory_order_relaxed))
        return;
    log_info3("remote send error (", strerror(err), "), will reconnect");
    int rfd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
    if (rfd >= 0) shutdown(rfd, SHUT_RDWR);
}

/* Callers classify the failure with note_remote_send_err(p, errno), so errno
 * has to survive the logging in between: strerror() and write() are both
 * allowed to clobber it, and the debug level is exactly the one someone turns
 * on to diagnose these errors. */
static int send_packet(int fd, const void *data, int len) {
    int r = (int)send(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (r < 0) {
        int err = errno;
        log_debug2("send_packet failed: ", strerror(err));
        errno = err;
    }
    return r;
}

static int send_packet_to(int fd, const void *data, int len, cliaddr_t *addr) {
    int r = (int)sendto(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL,
                        &addr->sa, cliaddr_len(addr));
    if (r < 0) {
        int err = errno;
        log_debug2("send_packet_to failed: ", strerror(err));
        errno = err;
    }
    return r;
}

static void send_junk_and_cps_to(proxy_t *p, int fd, cliaddr_t *addr) {
    awg_config_t *cfg = p->cfg;

    int ncps = cps_generate_all(cfg->cps, &p->cps_counter,
                                 p->cps_bufs, p->cps_lens);
    for (int i = 0; i < ncps; i++)
        send_packet_to(fd, p->cps_bufs[i], p->cps_lens[i], addr);

    if (cfg->jc > 0 && cfg->jmax > 0) {
        size_t junk_bytes;
        size_t junk_sizes_bytes;
        if (junk_layout_sizes(cfg, &junk_bytes, &junk_sizes_bytes) < 0)
            return;
        (void)junk_sizes_bytes;
        csprng_bytes(p->junk_buf, junk_bytes);
        int njunk = generate_junk(cfg, p->junk_buf, p->junk_sizes);
        size_t off = 0;
        for (int i = 0; i < njunk; i++) {
            send_packet_to(fd, p->junk_buf + off, p->junk_sizes[i], addr);
            off += (size_t)p->junk_sizes[i];
        }
    }
}

/* Remote-side counterpart of send_junk_and_cps_to(): fd is always the remote
 * socket here, so a fatal error on it is worth reporting. */
static void send_junk_and_cps(proxy_t *p, int fd) {
    awg_config_t *cfg = p->cfg;

    /* CPS packets */
    int ncps = cps_generate_all(cfg->cps, &p->cps_counter,
                                 p->cps_bufs, p->cps_lens);
    for (int i = 0; i < ncps; i++)
        if (send_packet(fd, p->cps_bufs[i], p->cps_lens[i]) < 0)
            note_remote_send_err(p, errno);

    /* Junk packets */
    if (cfg->jc > 0 && cfg->jmax > 0) {
        size_t junk_bytes;
        size_t junk_sizes_bytes;
        if (junk_layout_sizes(cfg, &junk_bytes, &junk_sizes_bytes) < 0)
            return;
        (void)junk_sizes_bytes;
        csprng_bytes(p->junk_buf, junk_bytes);
        int njunk = generate_junk(cfg, p->junk_buf, p->junk_sizes);
        size_t off = 0;
        for (int i = 0; i < njunk; i++) {
            if (send_packet(fd, p->junk_buf + off, p->junk_sizes[i]) < 0)
                note_remote_send_err(p, errno);
            off += (size_t)p->junk_sizes[i];
        }
    }
}

/* ---- Send batch with GSO ---- */

/* Returns errno of the failing send, or 0 when everything went out. Only the
 * remote path acts on it (see send_batch_remote) — a failed send to the local
 * WireGuard interface says nothing about the tunnel. */
static int send_batch_gso(proxy_t *p, int fd, struct mmsghdr *msgs,
                          struct iovec *iovecs, int nsend,
                          cliaddr_t *addr, int *sent_out) {
    int sent = 0;
    int err = 0;
    if (p->gso_ok && nsend > 1) {
        int n = send_gso(fd, iovecs, nsend, addr);
        if (n < 0) {
            err = -n;
            if (err == ENOPROTOOPT || err == EIO)
                p->gso_ok = 0;
        } else {
            sent = n;
            err = 0;
        }
    }
    while (sent < nsend) {
        int r = sendmmsg(fd, msgs + sent, nsend - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            err = errno;
            log_debug2("sendmmsg failed: ", strerror(err));
            break;
        }
        sent += r;
        err = 0;
    }
    if (sent_out) *sent_out = sent;
    return err;
}

/* Every c2s → remote flush goes through here, so the Happy Eyeballs probe gets
 * its copy without the per-packet fast path knowing anything about it. */
static inline void send_batch_remote(proxy_t *p, int fd, struct mmsghdr *msgs,
                                     struct iovec *iovecs, int nsend) {
    he_stash(p, iovecs[0].iov_base, (int)iovecs[0].iov_len, 0);
    if (!atomic_load_explicit(&p->fe_frag_warn, memory_order_relaxed)) {
        int v6 = (p->remote.sa.ss_family == AF_INET6);
        int outer = (int)iovecs[0].iov_len + (v6 ? 48 : 28);
        if (outer > 1500) log_frag_warn(p, v6, outer);
    }
    int sent = 0;
    int err = send_batch_gso(p, fd, msgs, iovecs, nsend, NULL, &sent);
    stats_add_tx(&p->st_c2s_tx, &p->st_c2s_drop, nsend, sent);
    if (err) note_remote_send_err(p, err);
}

/* ---- c2s thread ---- */

/* Latch the address a batch came from as "the client". Returns 1 only when it
 * changed — normal mode uses that to re-pin its source port. A family other
 * than the socket's means the kernel never filled msg_name, so there is
 * nothing to latch. */
static int note_client_addr(proxy_t *p, const cliaddr_t *a) {
    if (a->sa.sa_family != p->listen_family) return 0;
    if (atomic_load_explicit(&p->has_client, memory_order_acquire) &&
        cliaddr_eq(&p->client_addr, a))
        return 0;
    p->client_addr = *a;
    atomic_store_explicit(&p->has_client, 1, memory_order_release);
    int v6 = (a->sa.sa_family == AF_INET6);
    char abuf[INET6_ADDRSTRLEN], pbuf[12];
    const char *parts[] = { "client: ", v6 ? "[" : "",
                            sa_str(&a->sa, abuf, sizeof(abuf)), v6 ? "]:" : ":",
                            u32_to_str(pbuf, ntohs(cliaddr_port(a))) };
    log_infon(parts, 5);
    return 1;
}

/* ---- c2s: normal mode ---- */

__attribute__((hot))
static void *c2s_thread_normal(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    awg_config_t *cfg = p->cfg;
    set_thread_affinity(cfg->cpu_c2s, "c2s");
    const int prefix = p->c2s_headroom;
    int prev_nrecv = BATCH_SIZE;
    int gro_no_coalesce = 0;
    int gro_pend_off = 0, gro_pend_total = 0, gro_pend_seg = 0;
    int gro_rearm_in = 0, gro_rearm_gap = GRO_REARM_MIN;

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        int nrecv;

        if (p->gro_enabled_c2s) {
            ssize_t total;
            int seg_size;

            /* A 64 KiB read can hold more segments than one batch (the kernel
             * coalesces up to UDP_GRO_CNT_MAX = 64), so the remainder is kept
             * and drained on the next turn instead of being dropped. */
            if (gro_pend_off < gro_pend_total) {
                total = gro_pend_total;
                seg_size = gro_pend_seg;
            } else {
                p->gro_hdr_c2s.msg_controllen = sizeof(p->gro_cmsg_c2s);
                p->gro_hdr_c2s.msg_flags = 0;
                p->gro_hdr_c2s.msg_namelen = p->cli_len;

                total = spin_recvmsg(p, p->listen_fd, &p->gro_hdr_c2s);
                if (total <= 0) {
                    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;
                    continue;
                }
                seg_size = gro_seg_size(&p->gro_hdr_c2s);
                gro_pend_off = 0;
                gro_pend_total = 0;
            }

            /* Copy source address for client detection below */
            p->recv_c2s.addrs[0] = p->gro_addr_c2s;
            nrecv = 0;

            if (seg_size > 0 && total > seg_size) {
                int off = gro_pend_off;
                for (; off < total && nrecv < BATCH_SIZE; off += seg_size) {
                    int plen = (off + seg_size <= total) ? seg_size : (int)(total - off);
                    if (plen > BUF_SIZE) {
                        /* Cannot split a segment this large; give up on GRO
                         * rather than silently blackhole the direction. */
                        log_info("c2s: GRO segment larger than the packet buffer, disabling GRO");
                        p->gro_enabled_c2s = 0;
                        disable_gro(p->listen_fd);
                        nrecv = 0;
                        break;
                    }
                    memcpy(p->recv_c2s.bufs[nrecv] + prefix, p->gro_buf_c2s + off, plen);
                    p->recv_c2s.msgs[nrecv].msg_len = plen;
                    nrecv++;
                }
                if (nrecv > 0) gro_no_coalesce = 0;
                if (off < total) {
                    gro_pend_off = off;
                    gro_pend_total = (int)total;
                    gro_pend_seg = seg_size;
                } else {
                    gro_pend_off = gro_pend_total = 0;
                }
                if (nrecv == 0) continue;
            } else {
                if (++gro_no_coalesce >= 8) {
                    p->gro_enabled_c2s = 0;
                    disable_gro(p->listen_fd);
                    gro_rearm_in = gro_rearm_gap;
                    log_info("c2s: GRO not coalescing, falling back to recvmmsg");
                }
                if (total > BUF_SIZE) {
                    log_debug("c2s: dropping oversized GRO datagram");
                    continue;
                }
                memcpy(p->recv_c2s.bufs[0] + prefix, p->gro_buf_c2s, (int)total);
                p->recv_c2s.msgs[0].msg_len = (unsigned int)total;
                nrecv = 1;
            }
        } else {
            /* recvmmsg path */
            if (gro_rearm_in > 0 && --gro_rearm_in == 0 && !cfg->no_gro &&
                enable_gro(p->listen_fd)) {
                p->gro_enabled_c2s = 1;
                gro_no_coalesce = 0;
                if (gro_rearm_gap < GRO_REARM_MAX) gro_rearm_gap *= 2;
                log_info("c2s: retrying UDP GRO");
                continue;
            }
            for (int i = 0; i < prev_nrecv; i++)
                p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
            p->recv_c2s.msgs[0].msg_hdr.msg_namelen = p->cli_len;

            nrecv = spin_recvmmsg(p, p->listen_fd, p->recv_c2s.msgs, BATCH_SIZE);
            if (nrecv <= 0) {
                if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;
                continue;
            }
            prev_nrecv = nrecv;
        }

        atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);
        stats_add_rx(&p->st_c2s_rx, nrecv);

        /* Check client address from first packet */
        if (note_client_addr(p, &p->recv_c2s.addrs[0]) && p->auto_src_port) {
            int cp = ntohs(cliaddr_port(&p->recv_c2s.addrs[0]));
            if (p->local_port != cp) {
                p->local_port = cp;
                char pb2[12];
                log_info2("src port: auto, reconnecting port=",
                          u32_to_str(pb2, cp));
                atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
            }
        }

        int remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
        if (remote_fd < 0) continue;

        /* Build sendmmsg batch. Both flags are hoisted out of the loop, so a
         * v2 config pays one predicted branch per packet and no ChaCha20. */
        int nsend = 0;
        const int s4 = cfg->s4;
        const int hp = cfg->hp_on;

        for (int i = 0; i < nrecv; i++) {
            int n = (int)p->recv_c2s.msgs[i].msg_len;
            if (n <= 0) continue;

            uint8_t *data = p->recv_c2s.bufs[i] + prefix;

            /* Transport data fast-path */
            if (n >= WG_TRANSPORT_MIN) {
                uint32_t h;
                memcpy(&h, data, 4);
                if (h == WG_TRANSPORT_DATA) {
                    if (!cfg->h4_noop) {
                        uint32_t h4 = pick_h4(p);
                        memcpy(data, &h4, 4);
                    }
                    uint8_t *base = data;
                    int total = n;
                    if (s4 > 0) {
                        base = data - s4;
                        csprng_bytes(base, (size_t)s4);
                        total = s4 + n;
                    }
                    if (hp)
                        chacha20_xor(cfg->hp_key, base, data, AWG_HP_TRANSPORT_HDR);
                    awg_window_note(cfg, total);
                    p->send_c2s.iovecs[nsend].iov_base = base;
                    p->send_c2s.iovecs[nsend].iov_len = total;
                    nsend++;
                    if (!atomic_exchange_explicit(&p->fe_transport_c2s, 1, memory_order_relaxed))
                        log_info("c2s: first transport packet to remote");
                    continue;
                }
            }

            /* Handshake slow path: flush batch first */
            if (nsend > 0) {
                send_batch_remote(p, remote_fd, p->send_c2s.msgs,
                                  p->send_c2s.iovecs, nsend);
                nsend = 0;
            }

            /* Detect WG handshake init from client */
            if (n >= 4) {
                uint32_t hin;
                memcpy(&hin, data, 4);
                if (hin == WG_HANDSHAKE_INIT) {
                    /* Every init, not just the first: this is what tells the
                     * watchdog the tunnel is trying to come up rather than
                     * merely sitting idle. */
                    atomic_store_explicit(&p->client_init, 1, memory_order_relaxed);
                    if (!atomic_exchange_explicit(&p->fe_init_seen, 1, memory_order_relaxed)) {
                        char nb[12];
                        const char *parts[] = { "c2s: WG handshake init received from client (size=",
                                                u32_to_str(nb, n), ")" };
                        log_infon(parts, 3);
                    }
                }
            }

            int out_len, sendJunk;
            uint8_t *out = transform_outbound(p->recv_c2s.bufs[i], prefix, n,
                                               cfg, fastrand_u64(&p->rng_c2s),
                                               &out_len, &sendJunk);
            if (!out) {
                log_debug("c2s: cookie reply dropped (AWG_DISABLE_COOKIES)");
                continue;
            }

            if (sendJunk) {
                log_debug("c2s: handshake init, sending junk");
                send_junk_and_cps(p, remote_fd);
                /* The handshake init is the packet a server actually answers,
                 * so it is the one worth replaying on the other family. */
                he_stash(p, out, out_len, 1);
                if (send_packet(remote_fd, out, out_len) < 0)
                    note_remote_send_err(p, errno);
                if (!atomic_exchange_explicit(&p->fe_init_sent, 1, memory_order_relaxed)) {
                    char nb[12];
                    const char *parts[] = { "c2s: AWG handshake init forwarded to remote (size=",
                                            u32_to_str(nb, out_len), ")" };
                    log_infon(parts, 3);
                }
                continue;
            }

            /* Non-junk handshake */
            p->send_c2s.iovecs[nsend].iov_base = out;
            p->send_c2s.iovecs[nsend].iov_len = out_len;
            nsend++;
        }

        if (nsend > 0) {
            send_batch_remote(p, remote_fd, p->send_c2s.msgs,
                              p->send_c2s.iovecs, nsend);
        }
    }

    return NULL;
}

/* ---- c2s: reverse/server mode (AWG→WG inbound) ---- */

__attribute__((hot))
static void *c2s_thread_reverse(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    awg_config_t *cfg = p->cfg;
    int server_mode = (cfg->mode == AWG_MODE_SERVER);
    set_thread_affinity(cfg->cpu_c2s, "c2s");
    int prev_nrecv = BATCH_SIZE;

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        for (int i = 0; i < prev_nrecv; i++) {
            p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
            p->recv_c2s.msgs[i].msg_hdr.msg_namelen = p->cli_len;
        }

        int nrecv = spin_recvmmsg(p, p->listen_fd, p->recv_c2s.msgs, BATCH_SIZE);
        if (nrecv <= 0) {
            if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;
            continue;
        }
        prev_nrecv = nrecv;

        atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);
        stats_add_rx(&p->st_c2s_rx, nrecv);

        /* In reverse (1:1) mode, track single client */
        if (!server_mode)
            note_client_addr(p, &p->recv_c2s.addrs[0]);

        int remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
        if (remote_fd < 0) continue;

        /* Transform inbound (AWG→WG) and forward to WG server */
        int nsend = 0;
        const int multi = (cfg->profile_count > 1);

        for (int i = 0; i < nrecv; i++) {
            int n = (int)p->recv_c2s.msgs[i].msg_len;
            if (n <= 0) continue;

            uint8_t *pkt = p->recv_c2s.bufs[i];
            /* Server mode with a chain: each client keeps its own profile, so
             * clients on different AWG versions never override each other. */
            int prof = (multi && server_mode) ? profile_for_addr(p, &p->recv_c2s.addrs[i])
                                             : cfg->active_profile;
            const awg_profile_t *pr = &cfg->profiles[prof];
            const int s4 = pr->s4;
            awg_hp_ks_t ks = { .valid = 0 };

            /* Transport fast-path: strip S4 prefix, restore type */
            if (n >= s4 + WG_TRANSPORT_MIN) {
                if (!pr->transport_size_ambiguous ||
                    (n != pr->init_total && n != pr->resp_total && n != pr->cookie_total)) {
                    uint32_t h;
                    memcpy(&h, pkt + s4, 4);
                    const uint8_t *kstream = NULL;
                    if (pr->hp_on) {
                        kstream = hp_recv_ks(cfg, pkt, &ks);
                        uint32_t mask;
                        memcpy(&mask, kstream, 4);
                        h ^= mask;
                    }
                    if (hrange_contains(&pr->h4, h)) {
                        if (kstream)
                            chacha20_xor_ks(pkt + s4, kstream, AWG_HP_TRANSPORT_HDR);
                        uint32_t wt = WG_TRANSPORT_DATA;
                        memcpy(pkt + s4, &wt, 4);
                        awg_window_note(cfg, n);

                        /* Server mode: extract receiver_index for routing (but on c2s
                         * transport is from client, no need to record — server replies
                         * use receiver_index which maps to sender_index from init) */

                        p->send_c2s.iovecs[nsend].iov_base = pkt + s4;
                        p->send_c2s.iovecs[nsend].iov_len = n - s4;
                        nsend++;
                        continue;
                    }
                }
            }

            /* Handshake slow path: flush batch first */
            if (nsend > 0) {
                send_batch_remote(p, remote_fd, p->send_c2s.msgs,
                                  p->send_c2s.iovecs, nsend);
                nsend = 0;
            }

            int out_len;
            uint8_t *out = transform_inbound_profile(pkt, n, cfg, pr, &ks, &out_len);
            if (!out && multi) {
                /* Backward compat: the peer may be on another stage of the
                 * chain (an old v1/v2 site talking to a v3 config). Walk the
                 * chain; if something decodes, adopt that stage. The header
                 * protection keystream is shared — ChaCha20 runs once. */
                for (int k = 0; k < cfg->profile_count; k++) {
                    if (k == prof) continue;
                    out = transform_inbound_profile(pkt, n, cfg, &cfg->profiles[k],
                                                    &ks, &out_len);
                    if (!out) continue;
                    prof = k;
                    pr = &cfg->profiles[k];
                    if (server_mode) {
                        profile_remember(p, &p->recv_c2s.addrs[i], k);
                        log_debug("c2s: server: client adopted another profile stage");
                    } else {
                        switch_profile(p, k);
                        log_info("c2s: peer uses a different profile stage, switched");
                    }
                    break;
                }
            }
            if (!out) continue; /* junk packet, drop */

            /* Server mode: record sender_index from init/response for routing */
            if (server_mode && p->recv_c2s.addrs[i].sa.sa_family == p->listen_family) {
                uint32_t msg_type;
                memcpy(&msg_type, out, 4);
                if ((msg_type == WG_HANDSHAKE_INIT && out_len == WG_INIT_SIZE) ||
                    (msg_type == WG_HANDSHAKE_RESPONSE && out_len == WG_RESP_SIZE)) {
                    uint32_t sender_idx;
                    memcpy(&sender_idx, out + 4, 4);
                    session_put_prof(p, sender_idx, &p->recv_c2s.addrs[i], prof);
                    if (multi) profile_remember(p, &p->recv_c2s.addrs[i], prof);
                    log_debug("c2s: server: recorded handshake sender_index");
                }
            }

            p->send_c2s.iovecs[nsend].iov_base = out;
            p->send_c2s.iovecs[nsend].iov_len = out_len;
            nsend++;
        }

        if (nsend > 0) {
            send_batch_remote(p, remote_fd, p->send_c2s.msgs,
                              p->send_c2s.iovecs, nsend);
        }
    }

    return NULL;
}

static void *c2s_thread(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    if (p->cfg->mode != AWG_MODE_NORMAL)
        return c2s_thread_reverse(arg);
    return c2s_thread_normal(arg);
}

/* ---- s2c thread ---- */

/* ---- s2c packet processing: normal mode (AWG→WG inbound) ---- */

__attribute__((hot))
static inline int process_s2c_pkt_normal(proxy_t *p, uint8_t *pkt, int n,
                                          struct iovec *send_iovecs,
                                          cliaddr_t *send_addrs,
                                          int *nsend) {
    awg_config_t *cfg = p->cfg;
    int s4 = cfg->s4;
    awg_hp_ks_t ks = { .valid = 0 };

    /* Transport fast-path with precomputed ambiguity check */
    if (n >= s4 + WG_TRANSPORT_MIN) {
        if (!cfg->transport_size_ambiguous ||
            (n != cfg->init_total && n != cfg->resp_total && n != cfg->cookie_total)) {
            uint32_t h;
            memcpy(&h, pkt + s4, 4);
            const uint8_t *kstream = NULL;
            if (cfg->hp_on) {
                kstream = hp_recv_ks(cfg, pkt, &ks);
                uint32_t mask;
                memcpy(&mask, kstream, 4);
                h ^= mask;
            }
            if (hrange_contains(&cfg->h4, h)) {
                if (kstream)
                    chacha20_xor_ks(pkt + s4, kstream, AWG_HP_TRANSPORT_HDR);
                uint32_t wt = WG_TRANSPORT_DATA;
                memcpy(pkt + s4, &wt, 4);
                awg_window_note(cfg, n);
                int idx = *nsend;
                send_iovecs[idx].iov_base = pkt + s4;
                send_iovecs[idx].iov_len = n - s4;
                send_addrs[idx] = p->client_addr;
                (*nsend)++;
                if (!atomic_exchange_explicit(&p->fe_transport_s2c, 1, memory_order_relaxed))
                    log_info("s2c: first transport packet to client");
                return 1;
            }
        }
    }

    /* Slow path: handshake or unknown */
    int out_len;
    uint8_t *out = transform_inbound_profile(pkt, n, cfg, config_active_profile(cfg),
                                             &ks, &out_len);
    if (!out) {
        log_debug("s2c: junk packet dropped");
        return 0;
    }

    /* Identify handshake type for diagnostics */
    uint32_t mtype = 0;
    if (out_len >= 4) memcpy(&mtype, out, 4);
    if (g_log_level >= LOG_DEBUG) {
        const char *type_str = "unknown";
        if (mtype == WG_HANDSHAKE_INIT) type_str = "init";
        else if (mtype == WG_HANDSHAKE_RESPONSE) type_str = "resp";
        else if (mtype == WG_COOKIE_REPLY) type_str = "cookie";
        char nb[12];
        const char *parts[] = { "s2c: handshake type=", type_str, " size=", u32_to_str(nb, out_len) };
        log_debugn(parts, 4);
    }
    if (mtype == WG_HANDSHAKE_RESPONSE &&
        !atomic_exchange_explicit(&p->fe_resp_received, 1, memory_order_relaxed)) {
        char nb[12];
        const char *parts[] = { "s2c: AWG handshake response received from remote, transformed to WG (size=",
                                u32_to_str(nb, out_len), ")" };
        log_infon(parts, 3);
    }

    int idx = *nsend;
    send_iovecs[idx].iov_base = out;
    send_iovecs[idx].iov_len = out_len;
    send_addrs[idx] = p->client_addr;
    (*nsend)++;
    if (mtype == WG_HANDSHAKE_RESPONSE &&
        !atomic_exchange_explicit(&p->fe_resp_sent, 1, memory_order_relaxed))
        log_info("s2c: WG handshake response delivered to client");
    return 1;
}

/* ---- s2c packet processing: reverse/server mode (WG→AWG outbound) ---- */

__attribute__((hot))
static inline int process_s2c_pkt_reverse(proxy_t *p, uint8_t *base, uint8_t *pkt,
                                           int n, int prefix,
                                           struct iovec *send_iovecs,
                                           cliaddr_t *send_addrs,
                                           int *nsend) {
    awg_config_t *cfg = p->cfg;
    int server_mode = (cfg->mode == AWG_MODE_SERVER);

    /* Determine destination address */
    cliaddr_t *dest_addr = NULL;
    session_entry_t *dest_entry = NULL;
    uint32_t msg_type = 0;
    const uint8_t *out_mac1key = NULL;
    int init_peer = -1;
    if (server_mode) {
        /* Look up by receiver_index based on packet type */
        if (n < 4) return 0;
        memcpy(&msg_type, pkt, 4);

        if (msg_type == WG_TRANSPORT_DATA && n >= WG_TRANSPORT_MIN) {
            /* receiver_index at bytes 4-7 */
            uint32_t recv_idx;
            memcpy(&recv_idx, pkt + 4, 4);
            dest_entry = session_get_entry(p, recv_idx);
        } else if (msg_type == WG_HANDSHAKE_RESPONSE && n == WG_RESP_SIZE) {
            /* receiver_index at bytes 8-11 */
            uint32_t recv_idx;
            memcpy(&recv_idx, pkt + 8, 4);
            dest_entry = session_get_entry(p, recv_idx);
        } else if (msg_type == WG_COOKIE_REPLY && n == WG_COOKIE_SIZE) {
            /* receiver_index at bytes 4-7 */
            uint32_t recv_idx;
            memcpy(&recv_idx, pkt + 4, 4);
            dest_entry = session_get_entry(p, recv_idx);
        } else if (msg_type == WG_HANDSHAKE_INIT && n == WG_INIT_SIZE) {
            /* Server-initiated handshake: no receiver_index to look up. Its
             * MAC1 is keyed on the client's static key, so the peer list names
             * the target — the sole-client fallback below only ever worked for
             * a hub with exactly one client. */
            init_peer = config_server_resolve_peer_for_init(cfg, pkt, n);
            if (init_peer >= 0) {
                dest_entry = session_find_by_peer(p, init_peer);
                if (dest_entry)
                    log_debug("s2c: server: init routed by MAC1 to its peer");
            }
            if (!dest_entry) {
                dest_entry = session_find_sole_entry(p);
                if (dest_entry)
                    log_debug("s2c: server: init routed to sole client");
            }
        }
        if (!dest_entry) {
            log_debug("s2c: server: no session for packet, dropping");
            return 0;
        }
        dest_addr = &dest_entry->addr;
    } else {
        /* reverse 1:1: use single client_addr */
        if (!atomic_load_explicit(&p->has_client, memory_order_acquire)) return 0;
        dest_addr = &p->client_addr;
    }

    /* The reply must go out in the profile this client speaks. */
    int prof = cfg->active_profile;
    if (server_mode && cfg->profile_count > 1)
        prof = atomic_load_explicit(&dest_entry->prof, memory_order_relaxed);
    const awg_profile_t *pr = &cfg->profiles[prof];

    /* Transport fast-path */
    if (n >= WG_TRANSPORT_MIN) {
        uint32_t h;
        memcpy(&h, pkt, 4);
        if (h == WG_TRANSPORT_DATA) {
            if (!pr->h4_noop) {
                uint32_t h4 = pick_h4_prof(p, prof);
                memcpy(pkt, &h4, 4);
            }
            uint8_t *out = pkt;
            int total = n;
            if (pr->s4 > 0 && prefix >= pr->s4) {
                out = pkt - pr->s4;
                csprng_bytes(out, (size_t)pr->s4);
                total = pr->s4 + n;
                if (pr->hp_on)
                    chacha20_xor(cfg->hp_key, out, pkt, AWG_HP_TRANSPORT_HDR);
            }
            awg_window_note(cfg, total);
            int idx = *nsend;
            send_iovecs[idx].iov_base = out;
            send_iovecs[idx].iov_len = total;
            send_addrs[idx] = *dest_addr;
            (*nsend)++;
            return 1;
        }
    }

    /* Handshake slow path */
    if (server_mode) {
        /* A MAC1-resolved init already knows its peer, and knowing it here is
         * what lets the outgoing MAC1 be recomputed under the right key. */
        if (init_peer >= 0)
            session_set_peer_slot(dest_entry, init_peer);

        int peer_slot = session_get_peer_slot(dest_entry);
        if (peer_slot >= 0 && peer_slot < cfg->server_peer_count)
            out_mac1key = cfg->server_peer_mac1keys[peer_slot];

        if (msg_type == WG_HANDSHAKE_RESPONSE && n == WG_RESP_SIZE && !out_mac1key) {
            int resolved_peer = config_server_resolve_peer_for_response(cfg, pkt, n);
            if (resolved_peer >= 0) {
                session_set_peer_slot(dest_entry, resolved_peer);
                out_mac1key = cfg->server_peer_mac1keys[resolved_peer];
                peer_slot = resolved_peer;
            }
        }

        /* Every handshake is a fresh statement of where this peer lives, so it
         * is the moment to retire whatever it left at previous addresses. */
        if (peer_slot >= 0)
            session_drop_moved_peer(p, peer_slot, dest_addr);
    }

    int out_len, sendJunk;
    uint8_t *out = transform_outbound_profile(base, prefix, n, cfg, pr,
                                              out_mac1key,
                                              fastrand_u64(&p->rng_s2c),
                                              &out_len, &sendJunk);
    if (!out) {
        log_debug("s2c: cookie reply dropped (AWG_DISABLE_COOKIES)");
        return 0;
    }

    if (sendJunk) {
        log_debug("s2c: reverse: handshake init, sending junk");
        send_junk_and_cps_to(p, p->listen_fd, dest_addr);
        send_packet_to(p->listen_fd, out, out_len, dest_addr);
        return 1;
    }

    /* If the transform fell back to its shared buffer — here the headroom is
     * only max_s4, so every handshake with S1/S2/S3 above that lands there —
     * the next packet of the batch overwrites it before sendmmsg runs, and two
     * peers rekeying in one batch each get the other's packet. Send it now. */
    if (transform_is_shared_buf(out)) {
        send_packet_to(p->listen_fd, out, out_len, dest_addr);
        return 1;
    }

    int idx = *nsend;
    send_iovecs[idx].iov_base = out;
    send_iovecs[idx].iov_len = out_len;
    send_addrs[idx] = *dest_addr;
    (*nsend)++;
    return 1;
}

static int do_reconnect(proxy_t *p) {
    int old_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
    if (old_fd >= 0) {
        close(old_fd);
        atomic_store_explicit(&p->remote_fd, -1, memory_order_release);
    }
    /* A probe interrupted by reconnect_needed leaves its socket behind. */
    int old_fd2 = atomic_exchange_explicit(&p->remote_fd2, -1, memory_order_acq_rel);
    if (old_fd2 >= 0)
        close(old_fd2);

    log_info2("reconnecting to ", p->remote_host);

    int fd = dial_remote(p, 1);
    if (fd < 0) return -1;

    atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);
    atomic_store_explicit(&p->last_remote_rx, 0, memory_order_relaxed);
    atomic_store_explicit(&p->client_init, 0, memory_order_relaxed);
    if (p->cfg->mode == AWG_MODE_NORMAL)
        atomic_store_explicit(&p->has_client, 0, memory_order_release);
    atomic_store_explicit(&p->reconnect_needed, 0, memory_order_relaxed);
    /* The trailer window describes a path, and this is a new one — amneziawg
     * resets it the same way when a peer's endpoint changes. */
    atomic_store_explicit(&p->cfg->udp_window, AWG_DEFAULT_UDP_WINDOW,
                          memory_order_relaxed);

    /* Reset first-event flags so handshake phases re-log after reconnect */
    atomic_store_explicit(&p->fe_init_seen,     0, memory_order_relaxed);
    atomic_store_explicit(&p->fe_init_sent,     0, memory_order_relaxed);
    atomic_store_explicit(&p->fe_remote_pkt,    0, memory_order_relaxed);
    atomic_store_explicit(&p->fe_resp_received, 0, memory_order_relaxed);
    atomic_store_explicit(&p->fe_resp_sent,     0, memory_order_relaxed);
    atomic_store_explicit(&p->fe_transport_c2s, 0, memory_order_relaxed);
    atomic_store_explicit(&p->fe_transport_s2c, 0, memory_order_relaxed);

    atomic_store_explicit(&p->remote_fd, fd, memory_order_release);

    log_info2("reconnected to ", p->remote_host);
    return fd;
}

__attribute__((hot))
static void *s2c_thread(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    set_thread_affinity(p->cfg->cpu_s2c, "s2c");
    int reconnect_backoff = 1;
    int prev_nrecv = BATCH_SIZE;

    int reverse = (p->cfg->mode != AWG_MODE_NORMAL);

    /* Settle the address family before anything is attached to the socket:
     * GRO, and later every read, must land on the fd that survives the probe. */
    he_probe(p);

    /* Try to enable GRO on initial remote fd. Only in normal mode: the
     * coalesced-read path below is guarded by !reverse, and enabling the
     * sockopt without a reader that splits by segment size would hand merged
     * datagrams to recvmmsg as if they were one packet. */
    int remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
    if (remote_fd >= 0 && !p->cfg->no_gro && !reverse) {
        p->gro_enabled = enable_gro(remote_fd);
        if (p->gro_enabled)
            log_info("s2c: UDP GRO enabled");
    }
    if (p->cfg->no_gro)
        log_info("s2c: UDP GRO disabled (AWG_NO_GRO)");

    int s2c_headroom = p->s2c_headroom;
    int s2c_buflen = BUF_SIZE + AWG_PACKET_HEADROOM - s2c_headroom;
    int gro_no_coalesce = 0;
    int gro_rearm_in = 0, gro_rearm_gap = GRO_REARM_MIN;
    int gro_pend_off = 0, gro_pend_total = 0, gro_pend_seg = 0;

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);

        /* Reconnect if needed */
        if (remote_fd < 0 || atomic_load_explicit(&p->reconnect_needed, memory_order_relaxed)) {
            struct timespec slp = { .tv_sec = reconnect_backoff };
            nanosleep(&slp, NULL);
            if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;

            int new_fd = do_reconnect(p);
            if (new_fd < 0) {
                reconnect_backoff *= 2;
                if (reconnect_backoff > 30) reconnect_backoff = 30;
                log_error("reconnect failed, backing off");
                continue;
            }
            reconnect_backoff = 1;
            /* A fresh connection re-runs the probe: dial_remote() resolved the
             * name again, so this doubles as the re-resolve-on-failure path. */
            he_probe(p);
            remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
            if (remote_fd < 0) continue;
            /* Re-enable GRO on new fd */
            if (!p->cfg->no_gro && !reverse) {
                p->gro_enabled = enable_gro(remote_fd);
                if (p->gro_enabled)
                    log_info("s2c: UDP GRO re-enabled");
            }
            prev_nrecv = BATCH_SIZE;
            continue;
        }

        /* === Receive === */
        int nsend = 0;

        if (p->gro_enabled && !reverse) {
            /* GRO path: normal mode only (reverse uses outbound which needs headroom) */
            int seg_size, n;

            /* Drain any segments left over from the previous read first — one
             * 64 KiB coalesced read can carry more than BATCH_SIZE of them. */
            if (gro_pend_off < gro_pend_total) {
                n = gro_pend_total;
                seg_size = gro_pend_seg;
            } else {
                n = recv_gro(p, remote_fd, &seg_size);
                if (n <= 0) {
                    int saved_errno = errno;
                    if (n == 0 || (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK && saved_errno != EINTR)) {
                        if (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
                            log_info3("remote read error (", strerror(saved_errno), "), will reconnect");
                            atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                        }
                    }
                    continue;
                }
                gro_pend_off = 0;
                gro_pend_total = 0;
            }

            atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);
            atomic_store_explicit(&p->last_remote_rx, 1, memory_order_relaxed);
            reconnect_backoff = 1;

            if (!atomic_exchange_explicit(&p->fe_remote_pkt, 1, memory_order_relaxed)) {
                char nb[12];
                const char *parts[] = { "s2c: first packet received from remote (size=",
                                        u32_to_str(nb, n), ")" };
                log_infon(parts, 3);
            }

            if (!atomic_load_explicit(&p->has_client, memory_order_acquire)) continue;

            if (seg_size > 0 && n > seg_size) {
                gro_no_coalesce = 0;
                char nb[12], sb[12];
                log_debug3("s2c: GRO recv bytes=", u32_to_str(nb, n), u32_to_str(sb, seg_size));

                /* Coalesced: split buffer by seg_size */
                int off = gro_pend_off;
                for (; off < n && nsend < BATCH_SIZE; off += seg_size) {
                    int end = off + seg_size;
                    if (end > n) end = n;
                    int pkt_len = end - off;
                    process_s2c_pkt_normal(p, p->gro_buf + off, pkt_len,
                                           p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
                }
                if (off < n) {
                    gro_pend_off = off;
                    gro_pend_total = n;
                    gro_pend_seg = seg_size;
                } else {
                    gro_pend_off = gro_pend_total = 0;
                }
            } else {
                /* Single packet — GRO not coalescing */
                if (++gro_no_coalesce >= 8) {
                    p->gro_enabled = 0;
                    disable_gro(remote_fd);
                    gro_rearm_in = gro_rearm_gap;
                    log_info("s2c: GRO not coalescing, falling back to recvmmsg");
                }
                process_s2c_pkt_normal(p, p->gro_buf, n,
                                       p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
            }
        } else {
            /* Non-GRO path (or reverse mode): recvmmsg with MSG_WAITFORONE */
            if (gro_rearm_in > 0 && --gro_rearm_in == 0 && !reverse &&
                !p->cfg->no_gro && enable_gro(remote_fd)) {
                p->gro_enabled = 1;
                gro_no_coalesce = 0;
                if (gro_rearm_gap < GRO_REARM_MAX) gro_rearm_gap *= 2;
                log_info("s2c: retrying UDP GRO");
                continue;
            }
            for (int i = 0; i < prev_nrecv; i++)
                p->recv_s2c.iovecs[i].iov_len = s2c_buflen;

            int nrecv = spin_recvmmsg(p, remote_fd, p->recv_s2c.msgs, BATCH_SIZE);
            if (nrecv <= 0) {
                int saved_errno = errno;
                if (nrecv == 0 || (saved_errno != EAGAIN && saved_errno != EWOULDBLOCK && saved_errno != EINTR)) {
                    if (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
                        log_info3("remote read error (", strerror(saved_errno), "), will reconnect");
                        atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                    }
                }
                continue;
            }
            prev_nrecv = nrecv;

            atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);
            atomic_store_explicit(&p->last_remote_rx, 1, memory_order_relaxed);
            reconnect_backoff = 1;

            if (!atomic_exchange_explicit(&p->fe_remote_pkt, 1, memory_order_relaxed)) {
                int first_n = (int)p->recv_s2c.msgs[0].msg_len;
                char nb[12];
                const char *parts[] = { "s2c: first packet received from remote (size=",
                                        u32_to_str(nb, first_n), ")" };
                log_infon(parts, 3);
            }

            if (!reverse && !atomic_load_explicit(&p->has_client, memory_order_acquire)) continue;

            for (int i = 0; i < nrecv; i++) {
                int n = (int)p->recv_s2c.msgs[i].msg_len;
                if (n <= 0) continue;
                if (reverse) {
                    process_s2c_pkt_reverse(p, p->recv_s2c.bufs[i],
                                            p->recv_s2c.bufs[i] + s2c_headroom, n,
                                            s2c_headroom,
                                            p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
                } else {
                    process_s2c_pkt_normal(p, p->recv_s2c.bufs[i], n,
                                           p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
                }
            }
        }

        /* === Send === */
        if (nsend > 0) {
            cliaddr_t *gso_addr = (p->cfg->mode == AWG_MODE_SERVER)
                ? NULL : &p->send_s2c.addrs[0];
            int sent_s2c = 0;
            send_batch_gso(p, p->listen_fd, p->send_s2c.msgs,
                           p->send_s2c.iovecs, nsend, gso_addr, &sent_s2c);
            stats_add_tx(&p->st_s2c_tx, &p->st_s2c_drop, nsend, sent_s2c);
        }
    }

    return NULL;
}

/* ---- Main ---- */

/* ---- Self-tuning spin budget -------------------------------------------
 *
 * The loss this fights is a timing one: the router's WireGuard hands the proxy
 * a burst at LAN speed, and the couple of milliseconds it takes to wake the
 * reading thread is long enough to overrun a receive buffer that cannot be
 * raised past net.core.rmem_max from inside a container. Spinning removes the
 * wakeup from that path - the thread is already looking.
 *
 * How long to spin is not a constant. It depends on how the peer paces its
 * bursts, on what else the router is doing, and on how much of a core can be
 * spared for looking rather than working. So walk a ladder of budgets, score
 * each rung by the share of packets the kernel dropped for want of buffer,
 * settle on the best and re-check the others now and then. Drop share is the
 * only signal here not swamped by throughput noise, and it is the exact
 * failure being prevented. */
#define SP_LADDER_N     6
#define SP_START_IDX    3      /* SPIN_START_US, and the ladder must agree */
#define SP_WINDOW_TICKS 3      /* 5 s timer ticks per measurement window */
#define SP_MIN_PACKETS  20000  /* quieter than this and the window judges nothing:
                                * a light window drops nothing at any setting,
                                * so it would only vote for whatever is in force */
#define SP_PROBE_EVERY  20     /* settled windows between deliberate re-checks:
                                * a probe spends 15 s on a possibly worse rung,
                                * which is 5 % of the time at this spacing */

static const int sp_ladder[SP_LADDER_N] = { 0, 50, 100, 200, 400, 800 };

typedef struct {
    int idx;                        /* ladder rung in force */
    int best;                       /* lowest-scoring rung so far */
    int tick;                       /* 5 s ticks into the current window */
    int windows;                    /* judged windows since the last probe */
    int probe_cursor;
    unsigned score[SP_LADDER_N];    /* EWMA, drops per 10 000 packets */
    unsigned char seen[SP_LADDER_N];
    uint32_t pv_pkts;
    unsigned long long pv_drops;
} spin_ctl_t;

static uint32_t sp_packets(proxy_t *p) {
    /* s2c keeps no rx counter of its own; what it forwarded to the client is
     * the same count of datagrams it took off the remote socket. Kept in 32
     * bits on purpose: the difference between two samples is what matters, and
     * it survives the wrap. */
    return atomic_load_explicit(&p->st_c2s_rx, memory_order_relaxed) +
           atomic_load_explicit(&p->st_s2c_tx, memory_order_relaxed);
}

static unsigned long long sp_drops(proxy_t *p) {
    int rfd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
    return udp_socket_drops(p->listen_fd) +
           (rfd >= 0 ? udp_socket_drops(rfd) : 0);
}

static void sp_init(proxy_t *p, spin_ctl_t *c) {
    memset(c, 0, sizeof(*c));
    c->idx = SP_START_IDX;
    c->best = SP_START_IDX;
    c->pv_pkts = sp_packets(p);
    c->pv_drops = sp_drops(p);
}

/* One 5 s tick of the controller; acts once every SP_WINDOW_TICKS. */
static void sp_tick(proxy_t *p, spin_ctl_t *c) {
    if (++c->tick < SP_WINDOW_TICKS) return;
    c->tick = 0;

    uint32_t pkts = sp_packets(p);
    unsigned long long drops = sp_drops(p);
    uint32_t d_pkts = pkts - c->pv_pkts;          /* correct across the wrap */
    unsigned long long d_drops = drops > c->pv_drops ? drops - c->pv_drops : 0;
    c->pv_pkts = pkts;
    c->pv_drops = drops;

    /* An idle window makes every rung look perfect. Averaging that in would
     * erase what the busy windows measured, so it counts as no sample at all. */
    if (d_pkts < SP_MIN_PACKETS) return;

    unsigned sample = (unsigned)((d_drops * 10000ULL) / d_pkts);
    c->score[c->idx] = c->seen[c->idx] ? (c->score[c->idx] * 3 + sample) / 4
                                       : sample;
    c->seen[c->idx] = 1;

    /* Try every rung once before trusting any comparison, opening at the one
     * that measured best on the routers this was built for. */
    for (int k = 0; k < SP_LADDER_N; k++) {
        int i = (SP_START_IDX + k) % SP_LADDER_N;
        if (!c->seen[i]) {
            c->idx = i;
            atomic_store_explicit(&p->spin_us, sp_ladder[i], memory_order_relaxed);
            return;
        }
    }

    /* Ties go to the lower rung, and the ladder is ascending: when two budgets
     * both come through a loaded window without dropping anything, the cheaper
     * one wins and the router stops burning a core on looking for work that
     * was not going to be missed anyway. */
    int best = 0;
    for (int i = 1; i < SP_LADDER_N; i++)
        if (c->score[i] < c->score[best]) best = i;

    int next = best;
    if (++c->windows >= SP_PROBE_EVERY) {
        c->windows = 0;
        for (int k = 0; k < SP_LADDER_N; k++) {
            int i = (c->probe_cursor + k) % SP_LADDER_N;
            if (i != best) { next = i; c->probe_cursor = i + 1; break; }
        }
    }

    if (best != c->best) {
        char b[2][12];
        const char *parts[] = { "spin: settled on ",
                                u32_to_str(b[0], sp_ladder[best]), "us at ",
                                u32_to_str(b[1], c->score[best]), " drops/10k" };
        log_infon(parts, 5);
        c->best = best;
    }
    if (next != c->idx) {
        char b[2][12];
        const char *parts[] = { "spin: ", u32_to_str(b[0], sp_ladder[next]),
                                "us (was ", u32_to_str(b[1], sp_ladder[c->idx]),
                                "us)" };
        log_debugn(parts, 5);
        c->idx = next;
        atomic_store_explicit(&p->spin_us, sp_ladder[next], memory_order_relaxed);
    }
}

int proxy_run(proxy_t *p) {
    awg_config_t *cfg = p->cfg;

    /* Create listen socket (blocking for c2s thread). Family comes from
     * AWG_LISTEN; on a v6 socket V6ONLY is turned off so one hub can serve
     * clients of both families — a v4 one simply arrives v4-mapped. */
    p->listen_fd = create_udp_socket(p->listen_family, 1);
    if (p->listen_fd < 0) {
        log_error("socket create failed");
        return -1;
    }
    int opt = 1;
    setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (p->listen_family == AF_INET6) {
        int off = 0;
        setsockopt(p->listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
    }
    if (bind(p->listen_fd, &p->listen_addr.sa, p->cli_len) < 0) {
        log_error2("bind failed: ", strerror(errno));
        close(p->listen_fd);
        return -1;
    }
    if (p->listen_family == AF_INET6)
        log_ipv6_mtu_hint(p, "clients reach this proxy over IPv6");
    set_socket_buffers(p->listen_fd, cfg->socket_buf);
    atomic_store_explicit(&p->spin_us, cfg->spin_us, memory_order_relaxed);
    set_busy_poll(p->listen_fd, cfg->busy_poll);
    if (cfg->no_df)
        set_df_off(p->listen_fd, p->listen_family);
    /* Normal mode only — c2s_thread_reverse() reads with recvmmsg and has no
     * way to split a coalesced buffer back into datagrams. */
    if (!cfg->no_gro && cfg->mode == AWG_MODE_NORMAL) {
        p->gro_enabled_c2s = enable_gro(p->listen_fd);
        if (p->gro_enabled_c2s)
            log_info("c2s: UDP GRO enabled");
    }
    log_socket_buffers(p->listen_fd, cfg, "listen");

    /* Connect to remote with infinite retry (DNS may not be ready at startup).
     * Signals are not yet blocked here, so SIGTERM/SIGINT terminate via default
     * handler — listen_fd is the only open resource and OS reclaims it. */
    int rfd = -1;
    int backoff = 1;
    for (;;) {
        rfd = dial_remote(p, 1);
        if (rfd >= 0) break;
        char db[12];
        log_error2("initial connect failed, retrying in ", u32_to_str(db, backoff));
        struct timespec slp = { .tv_sec = backoff };
        nanosleep(&slp, NULL);
        backoff *= 2;
        if (backoff > 30) backoff = 30;
    }
    atomic_store_explicit(&p->remote_fd, rfd, memory_order_release);
    log_socket_buffers(rfd, cfg, "remote");
    atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);

    /* Signal handling */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    p->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (p->signal_fd < 0) {
        log_error("signalfd failed");
        return -1;
    }

    /* Timer fd for timeout checks (every 5 seconds) */
    p->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (p->timer_fd < 0) {
        log_error("timerfd failed");
        return -1;
    }
    struct itimerspec ts = {
        .it_interval = { .tv_sec = 5 },
        .it_value = { .tv_sec = 5 },
    };
    timerfd_settime(p->timer_fd, 0, &ts, NULL);

    /* Launch c2s and s2c threads */
    pthread_t t_c2s, t_s2c;
    pthread_create(&t_c2s, NULL, c2s_thread, p);
    pthread_create(&t_s2c, NULL, s2c_thread, p);

    /* Main thread: signal handling + timeout */
    int timeout_secs = cfg->timeout > 0 ? cfg->timeout : 180;
    int checks_needed = timeout_secs / 5;
    if (checks_needed < 1) checks_needed = 1;
    int silent_ticks = 0;      /* consecutive ticks with nothing from the remote */
    int init_unanswered = 0;   /* ...during which the client asked to handshake */

    /* Port hopping. When AWG_REMOTE names more than one port, a port that a
     * DPI box has cut must not cost the full AWG_TIMEOUT: three ticks is
     * 15 seconds, room for three WireGuard handshake retries on one port,
     * after which the next port gets its turn. A single-port remote keeps the
     * old behaviour exactly — hop_checks stays 0 and the branch is dead. */
#define PORT_HOP_CHECKS 3
    int hop_checks = (p->remote_ports.total > 1) ? PORT_HOP_CHECKS : 0;
    int hop_rounds = 0;

    /* Fallback probing (initiator side of a dual-profile s2s config): after
     * fb_after seconds of the client sending but the remote staying silent,
     * switch to the other profile and reconnect. Biased to the primary — we
     * only leave it once it demonstrably fails, and lock onto whichever
     * profile the remote answers on. */
    int fb_enabled = (cfg->profile_count > 1 && cfg->mode == AWG_MODE_NORMAL);
    int fb_checks = cfg->fb_after > 0 ? cfg->fb_after / 5 : 4;
    if (fb_checks < 1) fb_checks = 1;
    int fb_silent_count = 0;

    /* Periodic DNS re-resolve: hostname remotes only. Reconnect when the
     * current IP disappears from the A records on two consecutive checks
     * (round-robin resolvers may return a rotating subset). */
    int dns_checks = 0;
    if (cfg->dns_refresh > 0) {
        struct in6_addr lit;
        if (inet_pton(AF_INET, p->remote_host, &lit) != 1 &&
            inet_pton(AF_INET6, p->remote_host, &lit) != 1) {
            dns_checks = cfg->dns_refresh / 5;
            if (dns_checks < 1) dns_checks = 1;
            log_info("periodic DNS re-resolve enabled");
        }
    }
    int dns_tick = 0;
    int dns_miss = 0;

    /* Stats ticker: AWG_STATS seconds, rounded to the 5 s timer. Prints only
     * when something moved, so an idle tunnel stays silent in the router log. */
    int stats_checks = cfg->stats_interval > 0 ? cfg->stats_interval / 5 : 0;
    if (cfg->stats_interval > 0 && stats_checks < 1) stats_checks = 1;
    int stats_tick = 0;
    spin_ctl_t spc;
    if (cfg->spin_auto) {
        sp_init(p, &spc);
        log_info("spin: self-tuning enabled");
    }
    uint32_t pv_c2s_rx = 0, pv_c2s_tx = 0, pv_c2s_dr = 0;
    uint32_t pv_s2c_tx = 0, pv_s2c_dr = 0;
    udp_kstats_t pv_k = { 0, 0, 0 };
    unsigned long long pv_sl_l = 0, pv_sl_r = 0;
    if (stats_checks > 0) {
        read_udp_kstats(&pv_k);
        pv_sl_l = udp_socket_drops(p->listen_fd);
    }

    /* Epoll for signal + timer only */
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        log_error("epoll_create failed");
        atomic_store_explicit(&p->stopped, 1, memory_order_relaxed);
        goto join;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = p->signal_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, p->signal_fd, &ev);
    ev.data.fd = p->timer_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, p->timer_fd, &ev);

    struct epoll_event events[2];

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        int nev = epoll_wait(epfd, events, 2, 1000);
        if (nev < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nev; i++) {
            int fd = events[i].data.fd;

            if (fd == p->signal_fd) {
                struct signalfd_siginfo si;
                read(p->signal_fd, &si, sizeof(si));
                log_info("shutting down");
                atomic_store_explicit(&p->stopped, 1, memory_order_relaxed);
                break;
            }

            if (fd == p->timer_fd) {
                uint64_t expirations;
                read(p->timer_fd, &expirations, sizeof(expirations));
                int had_activity = atomic_exchange_explicit(&p->last_active, 0, memory_order_relaxed);
                int had_remote_rx = atomic_exchange_explicit(&p->last_remote_rx, 0, memory_order_relaxed);
                int had_init = atomic_exchange_explicit(&p->client_init, 0, memory_order_relaxed);

                if (fb_enabled) {
                    if (had_remote_rx) {
                        fb_silent_count = 0; /* current profile works, stay on it */
                    } else if (had_activity) {
                        /* Client sending, remote silent — the other side may be
                         * on a different profile. Probe by switching. */
                        if (++fb_silent_count >= fb_checks) {
                            int next = cfg->active_profile + 1;
                            if (next >= cfg->profile_count) next = 0;
                            switch_profile(p, next);
                            {
                                char pb[12];
                                const char *parts[] = { "fallback: remote silent, trying profile stage ",
                                                        u32_to_str(pb, next) };
                                log_infon(parts, 2);
                            }
                            fb_silent_count = 0;
                            /* The profile switch reconnects on its own; the
                             * silence watchdog must not also count this stretch
                             * against the new profile it has not yet tried. */
                            silent_ticks = 0;
                            init_unanswered = 0;
                            int rfd2 = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                            if (rfd2 >= 0) {
                                atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                                shutdown(rfd2, SHUT_RDWR);
                            }
                        }
                    }
                }

                /* One counter, driven by the remote's silence alone. The pair it
                 * replaced each zeroed the other's condition, so any traffic
                 * sparser than one packet per tick — a 25 s WireGuard keepalive,
                 * say — kept both pinned near zero and the watchdog never fired
                 * at all, leaving a wedged tunnel wedged indefinitely.
                 *
                 * Silence on its own is not a fault: an established tunnel with
                 * nothing to carry is legitimately quiet, and keepalives draw no
                 * reply. A handshake init does — WireGuard answers one whenever
                 * it can — so an init that goes unanswered for the whole timeout
                 * is the unambiguous signal that the path, not the traffic, has
                 * stopped. That is when the run may drop what it learned. */
                if (had_remote_rx) {
                    silent_ticks = 0;
                    init_unanswered = 0;
                    hop_rounds = 0;
                } else {
                    if (had_init) init_unanswered = 1;
                    if (silent_ticks < checks_needed) silent_ticks++;
                    if (hop_checks && init_unanswered && silent_ticks >= hop_checks) {
                        log_info("no answer on this port, trying another one");
                        /* he_reset means "the family we settled on may be the
                         * dead one" — a verdict that needs a long silence, not
                         * one short hop. Raise it once per AWG_TIMEOUT worth of
                         * hops so the hop keeps its own, faster clock. */
                        if (++hop_rounds * hop_checks >= checks_needed) {
                            hop_rounds = 0;
                            atomic_store_explicit(&p->he_reset, 1, memory_order_relaxed);
                        }
                        int rfd2 = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                        if (rfd2 >= 0) {
                            atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                            shutdown(rfd2, SHUT_RDWR);
                        }
                        silent_ticks = 0;
                        init_unanswered = 0;
                    } else if (silent_ticks >= checks_needed && init_unanswered) {
                        log_info("remote silent while the tunnel is handshaking, "
                                 "triggering reconnect");
                        atomic_store_explicit(&p->he_reset, 1, memory_order_relaxed);
                        int rfd2 = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                        if (rfd2 >= 0) {
                            atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                            shutdown(rfd2, SHUT_RDWR);
                        }
                        silent_ticks = 0;
                        init_unanswered = 0;
                    }
                }

                if (cfg->spin_auto) sp_tick(p, &spc);

                if (stats_checks > 0 && ++stats_tick >= stats_checks) {
                    stats_tick = 0;
                    uint32_t c2s_rx = atomic_load_explicit(&p->st_c2s_rx, memory_order_relaxed);
                    uint32_t c2s_tx = atomic_load_explicit(&p->st_c2s_tx, memory_order_relaxed);
                    uint32_t c2s_dr = atomic_load_explicit(&p->st_c2s_drop, memory_order_relaxed);
                    uint32_t s2c_tx = atomic_load_explicit(&p->st_s2c_tx, memory_order_relaxed);
                    uint32_t s2c_dr = atomic_load_explicit(&p->st_s2c_drop, memory_order_relaxed);
                    udp_kstats_t k = { 0, 0, 0 };
                    read_udp_kstats(&k);
                    uint32_t d_rx = c2s_rx - pv_c2s_rx, d_tx = c2s_tx - pv_c2s_tx;
                    uint32_t d_dr = c2s_dr - pv_c2s_dr;
                    uint32_t d_stx = s2c_tx - pv_s2c_tx, d_sdr = s2c_dr - pv_s2c_dr;
                    unsigned long long d_kin = k.in_errors - pv_k.in_errors;
                    unsigned long long d_krb = k.rcvbuf_errors - pv_k.rcvbuf_errors;
                    unsigned long long d_ksb = k.sndbuf_errors - pv_k.sndbuf_errors;
                    /* Which of the two sockets actually overflowed: during an
                     * upload the listen socket carries the data and the remote
                     * one only the ACKs, and the netns-wide counter cannot tell
                     * them apart. */
                    int rfd_now = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                    unsigned long long sl = udp_socket_drops(p->listen_fd);
                    unsigned long long sr = rfd_now >= 0 ? udp_socket_drops(rfd_now) : pv_sl_r;
                    unsigned long long d_sl = sl > pv_sl_l ? sl - pv_sl_l : 0;
                    unsigned long long d_sr = sr > pv_sl_r ? sr - pv_sl_r : 0;
                    pv_sl_l = sl; pv_sl_r = sr;
                    if (d_rx || d_tx || d_dr || d_stx || d_sdr ||
                        d_kin || d_krb || d_ksb || d_sl || d_sr) {
                        char b[11][12];
                        const char *parts[] = {
                            "stats: c2s rx=", u32_to_str(b[0], (unsigned)d_rx),
                            " tx=", u32_to_str(b[1], (unsigned)d_tx),
                            " drop=", u32_to_str(b[2], (unsigned)d_dr),
                            " | s2c tx=", u32_to_str(b[3], (unsigned)d_stx),
                            " drop=", u32_to_str(b[4], (unsigned)d_sdr),
                            " | kernel udp in_err=", u32_to_str(b[5], (unsigned)d_kin),
                            " rcvbuf_err=", u32_to_str(b[6], (unsigned)d_krb),
                            " sndbuf_err=", u32_to_str(b[7], (unsigned)d_ksb),
                            " | sockdrop listen=", u32_to_str(b[8], (unsigned)d_sl),
                            " remote=", u32_to_str(b[9], (unsigned)d_sr),
                            " | spin=", u32_to_str(b[10], (unsigned)atomic_load_explicit(
                                                &p->spin_us, memory_order_relaxed)) };
                        log_infon(parts, 22);
                    }
                    pv_c2s_rx = c2s_rx; pv_c2s_tx = c2s_tx; pv_c2s_dr = c2s_dr;
                    pv_s2c_tx = s2c_tx; pv_s2c_dr = s2c_dr; pv_k = k;
                }

                if (dns_checks > 0 && ++dns_tick >= dns_checks) {
                    dns_tick = 0;
                    int rfd2 = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                    if (rfd2 >= 0 &&
                        !atomic_load_explicit(&p->reconnect_needed, memory_order_relaxed)) {
                        int r = resolve_addr_check(p->remote_host,
                                                   (struct sockaddr *)&p->remote.sa);
                        if (r == 1 && ++dns_miss >= 2) {
                            dns_miss = 0;
                            log_info("DNS changed (current IP gone), triggering reconnect");
                            atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                            shutdown(rfd2, SHUT_RDWR);
                        } else if (r == 0) {
                            dns_miss = 0;
                        }
                        /* r == -1 (resolve error): keep the connection, retry later */
                    }
                }
            }
        }
    }

    close(epfd);

join:
    /* Stop threads by shutting down sockets */
    atomic_store_explicit(&p->stopped, 1, memory_order_relaxed);
    if (p->listen_fd >= 0)
        shutdown(p->listen_fd, SHUT_RDWR);
    rfd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
    if (rfd >= 0)
        shutdown(rfd, SHUT_RDWR);
    rfd = atomic_load_explicit(&p->remote_fd2, memory_order_acquire);
    if (rfd >= 0)
        shutdown(rfd, SHUT_RDWR);

    pthread_join(t_c2s, NULL);
    pthread_join(t_s2c, NULL);

    /* Cleanup */
    rfd = atomic_load_explicit(&p->remote_fd, memory_order_relaxed);
    if (rfd >= 0) close(rfd);
    rfd = atomic_load_explicit(&p->remote_fd2, memory_order_relaxed);
    if (rfd >= 0) close(rfd);
    if (p->listen_fd >= 0) close(p->listen_fd);
    if (p->signal_fd >= 0) close(p->signal_fd);
    if (p->timer_fd >= 0) close(p->timer_fd);
    if (p->he_evfd >= 0) close(p->he_evfd);
    free(p->junk_buf);
    free(p->junk_sizes);

    return 0;
}
