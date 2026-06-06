#include "proxy.h"
#include "cps.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

/* ---- Helpers ---- */

static void fill_h4_ring(proxy_t *p) {
    if (p->cfg->h4.min == p->cfg->h4.max) {
        uint32_t v = p->cfg->h4.min;
        for (int i = 0; i < H4_RING_SIZE; i++)
            p->h4_ring[i] = v;
        return;
    }
    for (int i = 0; i < H4_RING_SIZE; i++)
        p->h4_ring[i] = hrange_pick(&p->cfg->h4, fastrand_u64(&p->rng));
}

static inline uint32_t pick_h4(proxy_t *p) {
    uint32_t v = p->h4_ring[p->h4_idx];
    p->h4_idx = (p->h4_idx + 1) & (H4_RING_SIZE - 1);
    if (p->h4_idx == 0)
        fill_h4_ring(p);
    return v;
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

static int resolve_addr(const char *host, uint16_t port, struct sockaddr_in *addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr->sin_addr) == 1)
        return 0;

    log_info2("resolving ", host);

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *res;
    int gai_err = getaddrinfo(host, NULL, &hints, &res);
    if (gai_err != 0) {
        const char *parts[] = {"resolve ", host, ": ", gai_strerror(gai_err)};
        if (g_log_level >= LOG_ERROR) log_msgn("ERROR: ", parts, 4);
        return -1;
    }
    memcpy(addr, res->ai_addr, sizeof(*addr));
    addr->sin_port = htons(port);

    char ipbuf[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &addr->sin_addr, ipbuf, sizeof(ipbuf))) {
        const char *parts[] = { "resolved ", host, " -> ", ipbuf };
        log_infon(parts, 4);
    }

    freeaddrinfo(res);
    return 0;
}

static int parse_host_port(const char *s, char *host, int hostmax, uint16_t *port) {
    const char *colon = NULL;
    int len = 0;
    while (s[len]) len++;
    for (int i = len - 1; i >= 0; i--) {
        if (s[i] == ':') { colon = s + i; break; }
    }
    if (!colon) return -1;

    int hlen = (int)(colon - s);
    if (hlen >= hostmax) return -1;
    memcpy(host, s, hlen);
    host[hlen] = '\0';

    *port = 0;
    for (const char *p = colon + 1; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        *port = *port * 10 + (*p - '0');
    }
    return 0;
}

static int create_udp_socket(int blocking) {
    int flags = SOCK_DGRAM | SOCK_CLOEXEC;
    if (!blocking) flags |= SOCK_NONBLOCK;
    return socket(AF_INET, flags, 0);
}

static void set_socket_buffers(int fd, int size) {
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

static void set_busy_poll(int fd, int usec) {
    if (usec <= 0) return;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &usec, sizeof(usec));
#ifdef SO_BUSY_POLL_BUDGET
    int budget = BATCH_SIZE;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
#endif
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

static int dial_remote(proxy_t *p, int blocking) {
    if (resolve_addr(p->remote_host, p->remote_port, &p->remote_addr) < 0)
        return -1;

    int fd = create_udp_socket(blocking);
    if (fd < 0) return -1;

    if (p->local_port > 0) {
        struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(p->local_port) };
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
            log_error2("bind failed: ", strerror(errno));
            close(fd);
            return -1;
        }
    }

    if (connect(fd, (struct sockaddr *)&p->remote_addr, sizeof(p->remote_addr)) < 0) {
        log_error2("connect failed: ", strerror(errno));
        close(fd);
        return -1;
    }

    if (g_log_level >= LOG_INFO) {
        char ipbuf[INET_ADDRSTRLEN], pbuf[12];
        inet_ntop(AF_INET, &p->remote_addr.sin_addr, ipbuf, sizeof(ipbuf));
        const char *parts[] = { "connected to ", ipbuf, ":",
                                u32_to_str(pbuf, ntohs(p->remote_addr.sin_port)) };
        log_infon(parts, 4);
    }

    set_socket_buffers(fd, p->cfg->socket_buf);
    set_busy_poll(fd, p->cfg->busy_poll);
    return fd;
}

/* ---- GRO/GSO ---- */

static int enable_gro(int fd) {
    int val = 1;
    return setsockopt(fd, IPPROTO_UDP, UDP_GRO, &val, sizeof(val)) == 0;
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
    p->gro_hdr_c2s.msg_namelen = sizeof(struct sockaddr_in);
}

/* recv_gro: blocking recvmsg with GRO. Returns total bytes, sets *seg_size.
 * seg_size=0 means no coalescing (single packet). */
static int recv_gro(proxy_t *p, int fd, int *seg_size) {
    p->gro_hdr.msg_controllen = sizeof(p->gro_cmsg);
    p->gro_hdr.msg_flags = 0;

    ssize_t n = recvmsg(fd, &p->gro_hdr, 0);
    if (n <= 0) {
        *seg_size = 0;
        return (int)n;
    }

    *seg_size = 0;
    /* Parse cmsg for UDP_GRO segment size */
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&p->gro_hdr); cm; cm = CMSG_NXTHDR(&p->gro_hdr, cm)) {
        if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_SEGMENT) {
            uint16_t ss;
            memcpy(&ss, CMSG_DATA(cm), sizeof(ss));
            *seg_size = ss;
            break;
        }
    }

    return (int)n;
}

/* send_gso: send a prefix of same-size packets via one sendmsg with UDP_SEGMENT.
 * Returns number of packets sent, or negative errno on error. */
static int send_gso(int fd, struct iovec *iovecs, int count,
                    struct sockaddr_in *addr) {
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
        hdr.msg_namelen = sizeof(struct sockaddr_in);
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
    p->signal_fd = -1;
    p->timer_fd = -1;
    p->gso_ok = 1;

    if (config_validate(cfg, &cfg_err) < 0) {
        log_error2("invalid config: ", cfg_err);
        return -1;
    }

    /* Parse listen address */
    char host[256];
    uint16_t port;
    if (parse_host_port(listen_str, host, sizeof(host), &port) < 0)
        return -1;
    memset(&p->listen_addr, 0, sizeof(p->listen_addr));
    p->listen_addr.sin_family = AF_INET;
    p->listen_addr.sin_port = htons(port);
    if (host[0] && inet_pton(AF_INET, host, &p->listen_addr.sin_addr) != 1)
        p->listen_addr.sin_addr.s_addr = INADDR_ANY;

    /* Parse remote address */
    if (parse_host_port(remote_str, p->remote_host, sizeof(p->remote_host), &p->remote_port) < 0)
        return -1;

    if (src_port > 0) {
        p->local_port = src_port;
    } else {
        p->auto_src_port = 1;
    }

    /* Init PRNG */
    uint64_t seed;
    int ufd = open("/dev/urandom", O_RDONLY);
    if (ufd >= 0) {
        read(ufd, &seed, 8);
        close(ufd);
    } else {
        seed = (uint64_t)(uintptr_t)p ^ 0xDEADBEEFCAFEULL;
    }
    fastrand_init(&p->rng, seed);

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

    /* Init H4 ring */
    fill_h4_ring(p);

    /* Init batch I/O structures — invariant fields set once */
    int c2s_headroom = (cfg->mode == AWG_MODE_NORMAL) ? cfg->s4 : 0;
    int s2c_headroom = (cfg->mode == AWG_MODE_NORMAL) ? 0 : cfg->s4;
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
            p->recv_c2s.msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
        }
    } else {
        /* Normal: capture client addr only from first packet in batch */
        p->recv_c2s.msgs[0].msg_hdr.msg_name = &p->recv_c2s.addrs[0];
        p->recv_c2s.msgs[0].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
    }

    for (int i = 0; i < BATCH_SIZE; i++) {
        /* send_s2c: to listen socket with client addr */
        p->send_s2c.msgs[i].msg_hdr.msg_iov = &p->send_s2c.iovecs[i];
        p->send_s2c.msgs[i].msg_hdr.msg_iovlen = 1;
        p->send_s2c.msgs[i].msg_hdr.msg_name = &p->send_s2c.addrs[i];
        p->send_s2c.msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);

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

    /* Pre-fill S4 headroom with random data */
    if (c2s_headroom > 0) {
        for (int i = 0; i < BATCH_SIZE; i++)
            fastrand_fill(&p->rng, p->recv_c2s.bufs[i], c2s_headroom);
    }
    if (s2c_headroom > 0) {
        for (int i = 0; i < BATCH_SIZE; i++)
            fastrand_fill(&p->rng, p->recv_s2c.bufs[i], s2c_headroom);
    }

    /* Init GRO state */
    init_gro_state(p);

    return 0;
}

/* ---- Send helpers ---- */

static int send_packet(int fd, const void *data, int len) {
    int r = (int)send(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (r < 0)
        log_debug2("send_packet failed: ", strerror(errno));
    return r;
}

static int send_packet_to(int fd, const void *data, int len, struct sockaddr_in *addr) {
    int r = (int)sendto(fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL,
                        (struct sockaddr *)addr, sizeof(*addr));
    if (r < 0)
        log_debug2("send_packet_to failed: ", strerror(errno));
    return r;
}

static void send_junk_and_cps_to(proxy_t *p, int fd, struct sockaddr_in *addr) {
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
        fastrand_fill(&p->rng, p->junk_buf, junk_bytes);
        int njunk = generate_junk(cfg, p->junk_buf, p->junk_sizes);
        size_t off = 0;
        for (int i = 0; i < njunk; i++) {
            send_packet_to(fd, p->junk_buf + off, p->junk_sizes[i], addr);
            off += (size_t)p->junk_sizes[i];
        }
    }
}

static void send_junk_and_cps(proxy_t *p, int fd) {
    awg_config_t *cfg = p->cfg;

    /* CPS packets */
    int ncps = cps_generate_all(cfg->cps, &p->cps_counter,
                                 p->cps_bufs, p->cps_lens);
    for (int i = 0; i < ncps; i++)
        send_packet(fd, p->cps_bufs[i], p->cps_lens[i]);

    /* Junk packets */
    if (cfg->jc > 0 && cfg->jmax > 0) {
        size_t junk_bytes;
        size_t junk_sizes_bytes;
        if (junk_layout_sizes(cfg, &junk_bytes, &junk_sizes_bytes) < 0)
            return;
        (void)junk_sizes_bytes;
        fastrand_fill(&p->rng, p->junk_buf, junk_bytes);
        int njunk = generate_junk(cfg, p->junk_buf, p->junk_sizes);
        size_t off = 0;
        for (int i = 0; i < njunk; i++) {
            send_packet(fd, p->junk_buf + off, p->junk_sizes[i]);
            off += (size_t)p->junk_sizes[i];
        }
    }
}

/* ---- Send batch with GSO ---- */

static void send_batch_gso(proxy_t *p, int fd, struct mmsghdr *msgs,
                           struct iovec *iovecs, int nsend,
                           struct sockaddr_in *addr) {
    int sent = 0;
    if (p->gso_ok && nsend > 1) {
        int n = send_gso(fd, iovecs, nsend, addr);
        if (n < 0) {
            int err = -n;
            if (err == ENOPROTOOPT || err == EIO)
                p->gso_ok = 0;
        } else {
            sent = n;
        }
    }
    while (sent < nsend) {
        int r = sendmmsg(fd, msgs + sent, nsend - sent, MSG_NOSIGNAL);
        if (r <= 0) {
            log_debug2("sendmmsg failed: ", strerror(errno));
            break;
        }
        sent += r;
    }
}

/* ---- c2s thread ---- */

/* ---- c2s: normal mode ---- */

__attribute__((hot))
static void *c2s_thread_normal(void *arg) {
    proxy_t *p = (proxy_t *)arg;
    awg_config_t *cfg = p->cfg;
    set_thread_affinity(cfg->cpu_c2s, "c2s");
    int prefix = cfg->s4;
    int prev_nrecv = BATCH_SIZE;
    int gro_no_coalesce = 0;

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        int nrecv;

        if (p->gro_enabled_c2s) {
            /* GRO receive: coalesced packets into gro_buf_c2s */
            p->gro_hdr_c2s.msg_controllen = sizeof(p->gro_cmsg_c2s);
            p->gro_hdr_c2s.msg_flags = 0;
            p->gro_hdr_c2s.msg_namelen = sizeof(struct sockaddr_in);

            ssize_t total = recvmsg(p->listen_fd, &p->gro_hdr_c2s, 0);
            if (total <= 0) {
                if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;
                continue;
            }

            int seg_size = 0;
            for (struct cmsghdr *cm = CMSG_FIRSTHDR(&p->gro_hdr_c2s); cm;
                 cm = CMSG_NXTHDR(&p->gro_hdr_c2s, cm)) {
                if (cm->cmsg_level == IPPROTO_UDP && cm->cmsg_type == UDP_SEGMENT) {
                    uint16_t ss;
                    memcpy(&ss, CMSG_DATA(cm), sizeof(ss));
                    seg_size = ss;
                    break;
                }
            }

            /* Copy source address for client detection below */
            p->recv_c2s.addrs[0] = p->gro_addr_c2s;
            nrecv = 0;

            if (seg_size > 0 && total > seg_size) {
                gro_no_coalesce = 0;
                for (int off = 0; off < total && nrecv < BATCH_SIZE; off += seg_size) {
                    int plen = (off + seg_size <= total) ? seg_size : (int)(total - off);
                    if (plen > BUF_SIZE) {
                        log_debug("c2s: dropping oversized GRO segment");
                        continue;
                    }
                    memcpy(p->recv_c2s.bufs[nrecv] + prefix, p->gro_buf_c2s + off, plen);
                    p->recv_c2s.msgs[nrecv].msg_len = plen;
                    nrecv++;
                }
            } else {
                if (++gro_no_coalesce >= 8) {
                    p->gro_enabled_c2s = 0;
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
            for (int i = 0; i < prev_nrecv; i++)
                p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
            p->recv_c2s.msgs[0].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);

            nrecv = recvmmsg(p->listen_fd, p->recv_c2s.msgs, BATCH_SIZE,
                             MSG_WAITFORONE, NULL);
            if (nrecv <= 0) {
                if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;
                continue;
            }
            prev_nrecv = nrecv;
        }

        atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);

        /* Check client address from first packet */
        if (p->recv_c2s.addrs[0].sin_family == AF_INET) {
            if (!atomic_load_explicit(&p->has_client, memory_order_acquire) ||
                p->client_addr.sin_addr.s_addr != p->recv_c2s.addrs[0].sin_addr.s_addr ||
                p->client_addr.sin_port != p->recv_c2s.addrs[0].sin_port) {
                p->client_addr = p->recv_c2s.addrs[0];
                atomic_store_explicit(&p->has_client, 1, memory_order_release);
                char abuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &p->client_addr.sin_addr, abuf, sizeof(abuf));
                char pbuf[12];
                const char *parts[] = { "client: ", abuf, ":", u32_to_str(pbuf, ntohs(p->client_addr.sin_port)) };
                log_infon(parts, 4);

                if (p->auto_src_port) {
                    int cp = ntohs(p->recv_c2s.addrs[0].sin_port);
                    if (p->local_port != cp) {
                        p->local_port = cp;
                        char pb2[12];
                        log_info2("src port: auto, reconnecting port=",
                                  u32_to_str(pb2, cp));
                        atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                    }
                }
            }
        }

        int remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
        if (remote_fd < 0) continue;

        /* Build sendmmsg batch */
        int nsend = 0;

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
                    int total = prefix > 0 ? prefix + n : n;
                    uint8_t *base = prefix > 0 ? p->recv_c2s.bufs[i] : data;
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
                send_batch_gso(p, remote_fd, p->send_c2s.msgs,
                               p->send_c2s.iovecs, nsend, NULL);
                nsend = 0;
            }

            /* Detect WG handshake init from client */
            if (n >= 4) {
                uint32_t hin;
                memcpy(&hin, data, 4);
                if (hin == WG_HANDSHAKE_INIT &&
                    !atomic_exchange_explicit(&p->fe_init_seen, 1, memory_order_relaxed)) {
                    char nb[12];
                    const char *parts[] = { "c2s: WG handshake init received from client (size=",
                                            u32_to_str(nb, n), ")" };
                    log_infon(parts, 3);
                }
            }

            int out_len, sendJunk;
            uint8_t *out = transform_outbound(p->recv_c2s.bufs[i], prefix, n,
                                               cfg, fastrand_u64(&p->rng),
                                               &out_len, &sendJunk);

            if (sendJunk) {
                log_debug("c2s: handshake init, sending junk");
                send_junk_and_cps(p, remote_fd);
                send_packet(remote_fd, out, out_len);
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
            send_batch_gso(p, remote_fd, p->send_c2s.msgs,
                           p->send_c2s.iovecs, nsend, NULL);
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
    int s4 = cfg->s4;
    int prev_nrecv = BATCH_SIZE;

    while (!atomic_load_explicit(&p->stopped, memory_order_relaxed)) {
        for (int i = 0; i < prev_nrecv; i++) {
            p->recv_c2s.iovecs[i].iov_len = BUF_SIZE;
            p->recv_c2s.msgs[i].msg_hdr.msg_namelen = sizeof(struct sockaddr_in);
        }

        int nrecv = recvmmsg(p->listen_fd, p->recv_c2s.msgs, BATCH_SIZE,
                             MSG_WAITFORONE, NULL);
        if (nrecv <= 0) {
            if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) break;
            continue;
        }
        prev_nrecv = nrecv;

        atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);

        /* In reverse (1:1) mode, track single client */
        if (!server_mode && p->recv_c2s.addrs[0].sin_family == AF_INET) {
            if (!atomic_load_explicit(&p->has_client, memory_order_acquire) ||
                p->client_addr.sin_addr.s_addr != p->recv_c2s.addrs[0].sin_addr.s_addr ||
                p->client_addr.sin_port != p->recv_c2s.addrs[0].sin_port) {
                p->client_addr = p->recv_c2s.addrs[0];
                atomic_store_explicit(&p->has_client, 1, memory_order_release);
                char abuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &p->client_addr.sin_addr, abuf, sizeof(abuf));
                char pbuf[12];
                const char *parts[] = { "client: ", abuf, ":", u32_to_str(pbuf, ntohs(p->client_addr.sin_port)) };
                log_infon(parts, 4);
            }
        }

        int remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
        if (remote_fd < 0) continue;

        /* Transform inbound (AWG→WG) and forward to WG server */
        int nsend = 0;

        for (int i = 0; i < nrecv; i++) {
            int n = (int)p->recv_c2s.msgs[i].msg_len;
            if (n <= 0) continue;

            uint8_t *pkt = p->recv_c2s.bufs[i];

            /* Transport fast-path: strip S4 prefix, restore type */
            if (n >= s4 + WG_TRANSPORT_MIN) {
                if (!cfg->transport_size_ambiguous ||
                    (n != cfg->init_total && n != cfg->resp_total && n != cfg->cookie_total)) {
                    uint32_t h;
                    memcpy(&h, pkt + s4, 4);
                    if (hrange_contains(&cfg->h4, h)) {
                        uint32_t wt = WG_TRANSPORT_DATA;
                        memcpy(pkt + s4, &wt, 4);

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
                send_batch_gso(p, remote_fd, p->send_c2s.msgs,
                               p->send_c2s.iovecs, nsend, NULL);
                nsend = 0;
            }

            int out_len;
            uint8_t *out = transform_inbound(pkt, n, cfg, &out_len);
            if (!out) continue; /* junk packet, drop */

            /* Server mode: record sender_index from init/response for routing */
            if (server_mode && p->recv_c2s.addrs[i].sin_family == AF_INET) {
                uint32_t msg_type;
                memcpy(&msg_type, out, 4);
                if (msg_type == WG_HANDSHAKE_INIT && out_len == WG_INIT_SIZE) {
                    uint32_t sender_idx;
                    memcpy(&sender_idx, out + 4, 4);
                    session_put(p, sender_idx, &p->recv_c2s.addrs[i]);
                    log_debug("c2s: server: recorded init sender_index");
                } else if (msg_type == WG_HANDSHAKE_RESPONSE && out_len == WG_RESP_SIZE) {
                    uint32_t sender_idx;
                    memcpy(&sender_idx, out + 4, 4);
                    session_put(p, sender_idx, &p->recv_c2s.addrs[i]);
                    log_debug("c2s: server: recorded response sender_index");
                }
            }

            p->send_c2s.iovecs[nsend].iov_base = out;
            p->send_c2s.iovecs[nsend].iov_len = out_len;
            nsend++;
        }

        if (nsend > 0) {
            send_batch_gso(p, remote_fd, p->send_c2s.msgs,
                           p->send_c2s.iovecs, nsend, NULL);
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
                                          struct sockaddr_in *send_addrs,
                                          int *nsend) {
    awg_config_t *cfg = p->cfg;
    int s4 = cfg->s4;

    /* Transport fast-path with precomputed ambiguity check */
    if (n >= s4 + WG_TRANSPORT_MIN) {
        if (!cfg->transport_size_ambiguous ||
            (n != cfg->init_total && n != cfg->resp_total && n != cfg->cookie_total)) {
            uint32_t h;
            memcpy(&h, pkt + s4, 4);
            if (hrange_contains(&cfg->h4, h)) {
                uint32_t wt = WG_TRANSPORT_DATA;
                memcpy(pkt + s4, &wt, 4);
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
    uint8_t *out = transform_inbound(pkt, n, cfg, &out_len);
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
                                           struct sockaddr_in *send_addrs,
                                           int *nsend) {
    awg_config_t *cfg = p->cfg;
    int server_mode = (cfg->mode == AWG_MODE_SERVER);

    /* Determine destination address */
    struct sockaddr_in *dest_addr = NULL;
    session_entry_t *dest_entry = NULL;
    uint32_t msg_type = 0;
    const uint8_t *out_mac1key = NULL;
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
            /* Server-initiated rekey: no receiver_index, try sole client */
            dest_entry = session_find_sole_entry(p);
            if (dest_entry)
                log_debug("s2c: server: init routed to sole client");
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

    /* Transport fast-path */
    if (n >= WG_TRANSPORT_MIN) {
        uint32_t h;
        memcpy(&h, pkt, 4);
        if (h == WG_TRANSPORT_DATA) {
            if (!cfg->h4_noop) {
                uint32_t h4 = pick_h4(p);
                memcpy(pkt, &h4, 4);
            }
            int total = prefix > 0 ? prefix + n : n;
            uint8_t *out = prefix > 0 ? base : pkt;
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
        int peer_slot = session_get_peer_slot(dest_entry);
        if (peer_slot >= 0 && peer_slot < cfg->server_peer_count)
            out_mac1key = cfg->server_peer_mac1keys[peer_slot];

        if (msg_type == WG_HANDSHAKE_RESPONSE && n == WG_RESP_SIZE && !out_mac1key) {
            int resolved_peer = config_server_resolve_peer_for_response(cfg, pkt, n);
            if (resolved_peer >= 0) {
                session_set_peer_slot(dest_entry, resolved_peer);
                out_mac1key = cfg->server_peer_mac1keys[resolved_peer];
            }
        }
    }

    int out_len, sendJunk;
    uint8_t *out = transform_outbound_with_mac1(base, prefix, n, cfg,
                                                out_mac1key,
                                                fastrand_u64(&p->rng),
                                                &out_len, &sendJunk);

    if (sendJunk) {
        log_debug("s2c: reverse: handshake init, sending junk");
        send_junk_and_cps_to(p, p->listen_fd, dest_addr);
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

    log_info2("reconnecting to ", p->remote_host);

    int fd = dial_remote(p, 1);
    if (fd < 0) return -1;

    atomic_store_explicit(&p->last_active, 1, memory_order_relaxed);
    atomic_store_explicit(&p->last_remote_rx, 0, memory_order_relaxed);
    if (p->cfg->mode == AWG_MODE_NORMAL)
        atomic_store_explicit(&p->has_client, 0, memory_order_release);
    atomic_store_explicit(&p->reconnect_needed, 0, memory_order_relaxed);

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

    /* Try to enable GRO on initial remote fd */
    int remote_fd = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
    if (remote_fd >= 0 && !p->cfg->no_gro) {
        p->gro_enabled = enable_gro(remote_fd);
        if (p->gro_enabled)
            log_info("s2c: UDP GRO enabled");
    }
    if (p->cfg->no_gro)
        log_info("s2c: UDP GRO disabled (AWG_NO_GRO)");

    int reverse = (p->cfg->mode != AWG_MODE_NORMAL);
    int s2c_headroom = reverse ? p->cfg->s4 : 0;
    int s2c_buflen = BUF_SIZE + AWG_PACKET_HEADROOM - s2c_headroom;
    int gro_no_coalesce = 0;

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
            remote_fd = new_fd;
            /* Re-enable GRO on new fd */
            if (!p->cfg->no_gro) {
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
            int seg_size;
            int n = recv_gro(p, remote_fd, &seg_size);
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
                for (int off = 0; off < n && nsend < BATCH_SIZE; off += seg_size) {
                    int end = off + seg_size;
                    if (end > n) end = n;
                    int pkt_len = end - off;
                    process_s2c_pkt_normal(p, p->gro_buf + off, pkt_len,
                                           p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
                }
            } else {
                /* Single packet — GRO not coalescing */
                if (++gro_no_coalesce >= 8) {
                    p->gro_enabled = 0;
                    log_info("GRO not coalescing, falling back to recvmmsg");
                }
                process_s2c_pkt_normal(p, p->gro_buf, n,
                                       p->send_s2c.iovecs, p->send_s2c.addrs, &nsend);
            }
        } else {
            /* Non-GRO path (or reverse mode): recvmmsg with MSG_WAITFORONE */
            for (int i = 0; i < prev_nrecv; i++)
                p->recv_s2c.iovecs[i].iov_len = s2c_buflen;

            int nrecv = recvmmsg(remote_fd, p->recv_s2c.msgs, BATCH_SIZE,
                                 MSG_WAITFORONE, NULL);
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
            struct sockaddr_in *gso_addr = (p->cfg->mode == AWG_MODE_SERVER)
                ? NULL : &p->send_s2c.addrs[0];
            send_batch_gso(p, p->listen_fd, p->send_s2c.msgs,
                           p->send_s2c.iovecs, nsend, gso_addr);
        }
    }

    return NULL;
}

/* ---- Main ---- */

int proxy_run(proxy_t *p) {
    awg_config_t *cfg = p->cfg;

    /* Create listen socket (blocking for c2s thread) */
    p->listen_fd = create_udp_socket(1);
    if (p->listen_fd < 0) {
        log_error("socket create failed");
        return -1;
    }
    int opt = 1;
    setsockopt(p->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(p->listen_fd, (struct sockaddr *)&p->listen_addr,
             sizeof(p->listen_addr)) < 0) {
        log_error("bind failed");
        close(p->listen_fd);
        return -1;
    }
    set_socket_buffers(p->listen_fd, cfg->socket_buf);
    set_busy_poll(p->listen_fd, cfg->busy_poll);
    if (!cfg->no_gro) {
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
    int inactive_count = 0;
    int remote_silent_count = 0;

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
                if (had_activity) {
                    inactive_count = 0;
                    if (!had_remote_rx) {
                        /* Client sending but remote silent — DNS may have changed */
                        remote_silent_count++;
                        if (remote_silent_count >= checks_needed) {
                            log_info("remote silent (DNS re-resolve), triggering reconnect");
                            int rfd2 = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                            if (rfd2 >= 0) {
                                atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                                shutdown(rfd2, SHUT_RDWR);
                            }
                            remote_silent_count = 0;
                        }
                    } else {
                        remote_silent_count = 0;
                    }
                } else {
                    remote_silent_count = 0;
                    inactive_count++;
                    if (inactive_count >= checks_needed) {
                        log_info("remote timeout, triggering reconnect");
                        int rfd2 = atomic_load_explicit(&p->remote_fd, memory_order_acquire);
                        if (rfd2 >= 0) {
                            atomic_store_explicit(&p->reconnect_needed, 1, memory_order_relaxed);
                            shutdown(rfd2, SHUT_RDWR);
                        }
                        inactive_count = 0;
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

    pthread_join(t_c2s, NULL);
    pthread_join(t_s2c, NULL);

    /* Cleanup */
    rfd = atomic_load_explicit(&p->remote_fd, memory_order_relaxed);
    if (rfd >= 0) close(rfd);
    if (p->listen_fd >= 0) close(p->listen_fd);
    if (p->signal_fd >= 0) close(p->signal_fd);
    if (p->timer_fd >= 0) close(p->timer_fd);
    free(p->junk_buf);
    free(p->junk_sizes);

    return 0;
}
