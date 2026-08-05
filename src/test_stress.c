/*
 * Stress test for awg-proxy: fork/exec proxy, mock UDP endpoints,
 * craft WireGuard packets, measure packet loss under load.
 *
 * All traffic is 127.0.0.1 only. No real keys, no external connections.
 * Run: make test-stress (requires make build first)
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "test.h"
#include "transform.h"
#include "chacha20.h"

/* AWG parameters matching env vars passed to proxy */
#define TEST_JC   2
#define TEST_JMIN 50
#define TEST_JMAX 100
#define TEST_S1   20
#define TEST_S2   15
#define TEST_H1   1234567891u
#define TEST_H2   1234567892u
#define TEST_H3   1234567893u
#define TEST_H4   1234567894u

/* Non-zero keys to exercise MAC1 recompute path. NOT real keys. */
#define DUMMY_SERVER_PUB "AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="  /* 32 x 0x01 */
#define DUMMY_CLIENT_PUB "AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI="  /* 32 x 0x02 */

/* AWG 3.0 scenario: every padding must be >= 12 (the ChaCha20 nonce). */
#define TEST_V3_S1 20
#define TEST_V3_S2 15
#define TEST_V3_S3 18
#define TEST_V3_S4 14
#define TEST_HP_KEY_HEX \
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
static const uint8_t TEST_HP_KEY[32] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
};

#define BURST_COUNT     10000
#define MULTI_PER_CLIENT 2500
#define BIDIR_COUNT     5000
#define RECV_TIMEOUT_MS 4000
#define PROXY_BINARY    "../builds/awg-proxy"

/* ---- Helpers ---- */

static int find_free_port(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    int port = ntohs(a.sin_port);
    close(fd);
    return port;
}

static int make_udp_socket(int port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    /* Increase recv/send buffers for burst traffic */
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port)
    };
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

/* IPv6 loopback mock endpoint. Returns -1 when the host has no ::1 at all,
 * which the IPv6 scenarios treat as "skip", not "fail". */
static int make_udp_socket6(int port) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    a.sin6_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

static int make_client_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    return fd;
}

/* Unbound v6 client. -1 means the host has no IPv6 at all — skip, not fail. */
static int make_client_socket6(void) {
    int fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int bufsize = 8 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    return fd;
}

static struct sockaddr_in make_addr(int port) {
    struct sockaddr_in a = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port)
    };
    return a;
}

static struct sockaddr_in6 make_addr6(int port) {
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_loopback;
    a.sin6_port = htons(port);
    return a;
}

static char *itoa_buf(int v, char *buf) {
    snprintf(buf, 32, "%d", v);
    return buf;
}

static char *utoa_buf(uint32_t v, char *buf) {
    snprintf(buf, 32, "%u", v);
    return buf;
}

static int64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static long get_rss_kb(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long rss = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            sscanf(line + 6, "%ld", &rss);
            break;
        }
    }
    fclose(f);
    return rss;
}

/* listen_str and remote_str are passed to AWG_LISTEN / AWG_REMOTE verbatim, so
 * IPv6 scenarios can hand in "[::]:port", "[::1]:port" or a dual-stack name.
 * he_delay may be NULL to keep the default. */
/* The learned-family file is process-global state that outlives a scenario. Left
 * at its default every proxy start in the suite would read whatever the previous
 * scenario taught it, so "start on IPv4" would silently become "start on
 * whatever ran before". Each start gets its own empty file unless a scenario
 * pins one on purpose, which is how the multi-start cases keep continuity. */
static char g_state_file[96];
static int  g_state_pinned;

static void state_file_fresh(void) {
    static int seq;
    snprintf(g_state_file, sizeof(g_state_file), "/tmp/awg-state-%d-%d",
             (int)getpid(), seq++);
    unlink(g_state_file);
}

static void state_file_pin(void) {
    state_file_fresh();
    g_state_pinned = 1;
}

static void state_file_unpin(void) {
    g_state_pinned = 0;
}

/* Reads back what the run recorded: '6', '4', or 0 when nothing was written. */
static int state_file_read(void) {
    int fd = open(g_state_file, O_RDONLY);
    if (fd < 0) return 0;
    char c = 0;
    ssize_t n = read(fd, &c, 1);
    close(fd);
    return n == 1 ? c : 0;
}

/* Optional stderr capture — set to a path to keep the proxy's own log. */
static const char *g_proxy_log;
static const char *g_log_level = "error";
/* Silence watchdog period. Scenarios that need a reconnect on demand shorten it
 * so the outage path runs in seconds rather than the production three minutes. */
static const char *g_timeout = "30";
/* How often the run re-checks that its remote is still in the name's records.
 * Scenarios that move an address shorten it so the follow-up takes seconds. */
static const char *g_dns_refresh;

static pid_t start_proxy_listen_remote(const char *mode, const char *listen_str,
                                       const char *remote_str, const char *he_delay) {
    /* Find a free port for proxy's source to avoid auto_src_port reconnect race */
    int src_port = find_free_port();

    if (!g_state_pinned) state_file_fresh();

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8];
        char h1[16], h2[16], h3[16], h4[16];

        setenv("AWG_LISTEN", listen_str, 1);
        setenv("AWG_REMOTE", remote_str, 1);
        if (he_delay) setenv("AWG_HE_DELAY", he_delay, 1);
        else unsetenv("AWG_HE_DELAY");
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_S2, s2), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", g_log_level, 1);
        setenv("AWG_TIMEOUT", g_timeout, 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);
        setenv("AWG_STATE_FILE", g_state_file, 1);
        if (g_dns_refresh) setenv("AWG_DNS_REFRESH", g_dns_refresh, 1);
        else               unsetenv("AWG_DNS_REFRESH");

        if (g_proxy_log) {
            int lf = open(g_proxy_log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (lf >= 0) { dup2(lf, 2); close(lf); }
        }

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000); /* 200ms for proxy startup */
    return pid;
}

static pid_t start_proxy_remote(const char *mode, int listen_port,
                                const char *remote_str, const char *he_delay) {
    char lbuf[32];
    snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
    return start_proxy_listen_remote(mode, lbuf, remote_str, he_delay);
}

static pid_t start_proxy(const char *mode, int listen_port, int remote_port) {
    char rbuf[64];
    snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);
    return start_proxy_remote(mode, listen_port, rbuf, NULL);
}

static pid_t start_proxy_with_gro(const char *mode, int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8];
        char h1[16], h2[16], h3[16], h4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_S2, s2), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "30", 1);
        /* GRO enabled — no AWG_NO_GRO */
        unsetenv("AWG_NO_GRO");
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

/* AWG 3.0 proxy: same profile plus S3/S4 and a header protection key. */
static pid_t start_proxy_v3(const char *mode, int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8], s3[8], s4[8];
        char h1[16], h2[16], h3[16], h4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_V3_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_V3_S2, s2), 1);
        setenv("AWG_S3", itoa_buf(TEST_V3_S3, s3), 1);
        setenv("AWG_S4", itoa_buf(TEST_V3_S4, s4), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_HEADER_PROTECTION_KEY", TEST_HP_KEY_HEX, 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "30", 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

/* AWG 3.1 proxy: the v3 profile with random trailers and cookies disabled. */
static pid_t start_proxy_v31(const char *mode, int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8], s1[8], s2[8], s3[8], s4[8];
        char h1[16], h2[16], h3[16], h4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", mode, 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        setenv("AWG_S1", itoa_buf(TEST_V3_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(TEST_V3_S2, s2), 1);
        setenv("AWG_S3", itoa_buf(TEST_V3_S3, s3), 1);
        setenv("AWG_S4", itoa_buf(TEST_V3_S4, s4), 1);
        setenv("AWG_H1", utoa_buf(TEST_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(TEST_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(TEST_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(TEST_H4, h4), 1);
        setenv("AWG_HEADER_PROTECTION_KEY", TEST_HP_KEY_HEX, 1);
        setenv("AWG_RANDOM_TRAILERS", "on", 1);
        setenv("AWG_DISABLE_COOKIES", "on", 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "30", 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

static void stop_proxy(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    int st;
    waitpid(pid, &st, 0);
}

/* ---- Packet crafting ---- */

static void make_wg_init(uint8_t *buf, uint32_t sender_index) {
    memset(buf, 0, WG_INIT_SIZE);
    uint32_t t = WG_HANDSHAKE_INIT;
    memcpy(buf, &t, 4);
    memcpy(buf + 4, &sender_index, 4);
    for (int i = 8; i < WG_INIT_SIZE; i++)
        buf[i] = (uint8_t)(i ^ (sender_index & 0xFF));
}

/* WG handshake response: type, sender_index, receiver_index. The proxy routes
 * it by receiver_index and does not validate the crypto, so the rest is filler
 * keyed on sender_index — that is what tells two responses apart on arrival. */
static void make_wg_response(uint8_t *buf, uint32_t sender_index,
                             uint32_t receiver_index) {
    memset(buf, 0, WG_RESP_SIZE);
    uint32_t t = WG_HANDSHAKE_RESPONSE;
    memcpy(buf, &t, 4);
    memcpy(buf + 4, &sender_index, 4);
    memcpy(buf + 8, &receiver_index, 4);
    for (int i = 12; i < WG_RESP_SIZE; i++)
        buf[i] = (uint8_t)(i ^ (sender_index & 0xFF));
}

static void make_wg_transport(uint8_t *buf, uint32_t receiver_index,
                               uint64_t counter, int total_size) {
    memset(buf, 0, total_size);
    uint32_t t = WG_TRANSPORT_DATA;
    memcpy(buf, &t, 4);
    memcpy(buf + 4, &receiver_index, 4);
    memcpy(buf + 8, &counter, 8);
    for (int i = 16; i < total_size; i++)
        buf[i] = (uint8_t)(i ^ (counter & 0xFF));
}

/* AWG-format init: S1 padding + H1 type + init payload */
static void make_awg_init(uint8_t *buf, uint32_t sender_index) {
    for (int i = 0; i < TEST_S1; i++) buf[i] = (uint8_t)(i * 7);
    uint32_t h = TEST_H1;
    memcpy(buf + TEST_S1, &h, 4);
    memcpy(buf + TEST_S1 + 4, &sender_index, 4);
    for (int i = 8; i < WG_INIT_SIZE; i++)
        buf[TEST_S1 + i] = (uint8_t)(i ^ (sender_index & 0xFF));
}

/* make_awg_response not needed — burst test only needs transport */

/* AWG-format transport: H4 type + transport payload */
static void make_awg_transport(uint8_t *buf, uint32_t receiver_index,
                                uint64_t counter, int payload_size) {
    int total = payload_size;
    memset(buf, 0, total);
    uint32_t h = TEST_H4;
    memcpy(buf, &h, 4);
    memcpy(buf + 4, &receiver_index, 4);
    memcpy(buf + 8, &counter, 8);
    for (int i = 16; i < total; i++)
        buf[i] = (uint8_t)(i ^ (counter & 0xFF));
}

/* Receive one packet with timeout, return size or -1 */
static int recv_one(int fd, uint8_t *buf, int bufsize, int timeout_ms) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return -1;
    ssize_t n = recvfrom(fd, buf, bufsize, MSG_DONTWAIT, NULL, NULL);
    return (int)n;
}

/* Drain socket of all pending packets */
static void drain_socket(int fd) {
    uint8_t buf[2048];
    while (recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0);
}

/* Async receiver: runs in a thread, counts packets until stopped */
typedef struct {
    int fd;
    _Atomic int count;
    _Atomic int stop;
    /* For index tracking (server_multiclient routing check) */
    uint32_t *indices;     /* NULL if not tracking */
    int max_indices;
} async_recv_t;

static void *async_recv_thread(void *arg) {
    async_recv_t *r = (async_recv_t *)arg;
    uint8_t buf[2048];
    struct pollfd pfd = { .fd = r->fd, .events = POLLIN };

    while (!atomic_load(&r->stop)) {
        int ret = poll(&pfd, 1, 100);
        if (ret <= 0) continue;
        for (;;) {
            ssize_t n = recvfrom(r->fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
            if (n <= 0) break;
            int idx = atomic_fetch_add(&r->count, 1);
            if (r->indices && idx < r->max_indices && n >= 8) {
                uint32_t ri;
                memcpy(&ri, buf + 4, 4);
                r->indices[idx] = ri;
            }
        }
    }
    /* Final drain */
    for (;;) {
        ssize_t n = recvfrom(r->fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL);
        if (n <= 0) break;
        int idx = atomic_fetch_add(&r->count, 1);
        if (r->indices && idx < r->max_indices && n >= 8) {
            uint32_t ri;
            memcpy(&ri, buf + 4, 4);
            r->indices[idx] = ri;
        }
    }
    return NULL;
}

static void async_recv_start(async_recv_t *r, pthread_t *t, int fd) {
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    pthread_create(t, NULL, async_recv_thread, r);
}

static int async_recv_stop(async_recv_t *r, pthread_t t) {
    atomic_store(&r->stop, 1);
    pthread_join(t, NULL);
    return atomic_load(&r->count);
}

/* ---- Scenario 1: Normal mode burst throughput ---- */

static void test_normal_burst(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake: send init */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x1000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    /* Server: receive junk + transformed init; skip junk, find init */
    usleep(200000);
    uint8_t rbuf[2048];
    int init_received = 0;
    for (int i = 0; i < TEST_JC + 5; i++) {
        int n = recv_one(server_fd, rbuf, sizeof(rbuf), 500);
        if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) { init_received = 1; break; }
        }
    }
    ASSERT(init_received);
    drain_socket(server_fd);

    /* Start async receiver BEFORE sending burst */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    /* Burst: send transport packets.
     * Pace sends in batches of 16 (half proxy BATCH_SIZE=32) with
     * micro-pause to avoid kernel UDP buffer overflow on loopback. */
    uint8_t transport[200];
    for (int i = 0; i < BURST_COUNT; i++) {
        make_wg_transport(transport, 0x2000, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(100);
    }

    /* Wait for proxy to flush everything */
    usleep(500000);
    int received = async_recv_stop(&srv_recv, srv_thread);
    double loss = 100.0 * (BURST_COUNT - received) / BURST_COUNT;

    fprintf(stderr, "          (sent=%d, recv=%d, loss=%.2f%%)\n", BURST_COUNT, received, loss);
    ASSERT(received >= BURST_COUNT * 99 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 2: Reverse mode bidirectional ---- */

typedef struct {
    int fd;
    struct sockaddr_in dest;
    int count;
    int direction; /* 0=c2s, 1=s2c */
} bidir_args_t;

static void *bidir_sender(void *arg) {
    bidir_args_t *a = (bidir_args_t *)arg;
    uint8_t buf[200];

    for (int i = 0; i < a->count; i++) {
        if (a->direction == 0) {
            /* Client sends AWG transport to proxy */
            make_awg_transport(buf, 0x2000, (uint64_t)i, 200);
        } else {
            /* Server sends WG transport to proxy */
            make_wg_transport(buf, 0x1000, (uint64_t)i, 200);
        }
        sendto(a->fd, buf, 200, 0,
               (struct sockaddr *)&a->dest, sizeof(a->dest));
        if ((i + 1) % 64 == 0) usleep(100);
    }
    return NULL;
}

static void test_reverse_bidirectional(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("reverse", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake: client sends AWG init to register with proxy */
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
    make_awg_init(awg_init, 0x1000);
    sendto(client_fd, awg_init, sizeof(awg_init), 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(200000);

    /* Drain the init at server */
    {
        uint8_t tmp[2048];
        for (int i = 0; i < 5; i++)
            recv_one(server_fd, tmp, sizeof(tmp), 200);
    }

    /* Now send bidirectional transport */
    bidir_args_t c2s_args = { .fd = client_fd, .dest = proxy_addr,
                               .count = BIDIR_COUNT, .direction = 0 };
    bidir_args_t s2c_args = { .fd = server_fd, .dest = { 0 },
                               .count = BIDIR_COUNT, .direction = 1 };

    /* Capture proxy's source address from server side */
    {
        uint8_t probe[TEST_S1 + WG_INIT_SIZE];
        make_awg_init(probe, 0x1001);
        sendto(client_fd, probe, sizeof(probe), 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(100000);
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got_addr = 0;
        for (int i = 0; i < 10; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { s2c_args.dest = from; got_addr = 1; }
        }
        ASSERT(got_addr);
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Start async receivers BEFORE sending */
    async_recv_t srv_recv, cli_recv;
    pthread_t srv_thread, cli_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);
    async_recv_start(&cli_recv, &cli_thread, client_fd);

    /* Launch sender threads */
    pthread_t t_c2s, t_s2c;
    pthread_create(&t_c2s, NULL, bidir_sender, &c2s_args);
    pthread_create(&t_s2c, NULL, bidir_sender, &s2c_args);
    pthread_join(t_c2s, NULL);
    pthread_join(t_s2c, NULL);

    /* Wait for proxy to flush */
    usleep(500000);
    int c2s_recv = async_recv_stop(&srv_recv, srv_thread);
    int s2c_recv = async_recv_stop(&cli_recv, cli_thread);

    double c2s_loss = 100.0 * (BIDIR_COUNT - c2s_recv) / BIDIR_COUNT;
    double s2c_loss = 100.0 * (BIDIR_COUNT - s2c_recv) / BIDIR_COUNT;

    fprintf(stderr, "          (c2s: %d/%d, s2c: %d/%d, loss=%.2f%%/%.2f%%)\n",
            c2s_recv, BIDIR_COUNT, s2c_recv, BIDIR_COUNT, c2s_loss, s2c_loss);
    ASSERT(c2s_recv >= BIDIR_COUNT * 99 / 100);
    ASSERT(s2c_recv >= BIDIR_COUNT * 99 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 3: Server mode multi-client ---- */

static void test_server_multiclient(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("server", listen_port, remote_port);
    ASSERT(proxy > 0);

    struct sockaddr_in proxy_addr = make_addr(listen_port);
    int num_clients = 4;
    int client_fds[4];
    uint32_t sender_indices[4] = { 0x1000, 0x2000, 0x3000, 0x4000 };

    /* Create clients and do handshakes */
    for (int c = 0; c < num_clients; c++) {
        client_fds[c] = make_client_socket();
        ASSERT(client_fds[c] >= 0);

        /* Send AWG-format init (reverse/server mode: client sends AWG) */
        uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
        make_awg_init(awg_init, sender_indices[c]);
        sendto(client_fds[c], awg_init, sizeof(awg_init), 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(50000);
    }

    /* Server drains all handshakes/junk */
    usleep(300000);
    {
        uint8_t tmp[2048];
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Get proxy's source addr from server perspective */
    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        /* Re-send one init to capture addr */
        uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
        make_awg_init(awg_init, sender_indices[0]);
        sendto(client_fds[0], awg_init, sizeof(awg_init), 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(100000);
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n > 0) {
            proxy_remote_addr = from;
        }
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Start async receiver on server BEFORE sending */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    /* Each client sends transport packets */
    int total_sent = 0;
    for (int c = 0; c < num_clients; c++) {
        uint8_t buf[200];
        for (int i = 0; i < MULTI_PER_CLIENT; i++) {
            make_awg_transport(buf, sender_indices[c], (uint64_t)i, 200);
            sendto(client_fds[c], buf, 200, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            total_sent++;
            if ((total_sent) % 64 == 0) usleep(100);
        }
    }

    /* Wait for proxy to flush */
    usleep(500000);
    int server_recv = async_recv_stop(&srv_recv, srv_thread);

    /* Start async receivers on each client for routing check */
    async_recv_t cli_recvs[4];
    pthread_t cli_threads[4];
    uint32_t cli_indices[4][200];
    for (int c = 0; c < num_clients; c++) {
        memset(&cli_recvs[c], 0, sizeof(async_recv_t));
        cli_recvs[c].fd = client_fds[c];
        cli_recvs[c].indices = cli_indices[c];
        cli_recvs[c].max_indices = 200;
        pthread_create(&cli_threads[c], NULL, async_recv_thread, &cli_recvs[c]);
    }

    /* Server sends responses addressed to each client via receiver_index */
    for (int c = 0; c < num_clients; c++) {
        uint8_t buf[200];
        for (int i = 0; i < 100; i++) {
            make_wg_transport(buf, sender_indices[c], (uint64_t)i, 200);
            sendto(server_fd, buf, 200, 0,
                   (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
        }
        usleep(10000);
    }

    /* Wait and collect */
    usleep(500000);
    int routing_ok = 1;
    int total_client_recv = 0;
    for (int c = 0; c < num_clients; c++) {
        int cnt = async_recv_stop(&cli_recvs[c], cli_threads[c]);
        total_client_recv += cnt;
        for (int i = 0; i < cnt && i < 200; i++) {
            if (cli_indices[c][i] != sender_indices[c]) {
                routing_ok = 0;
                break;
            }
        }
    }

    double loss = 100.0 * (total_sent - server_recv) / total_sent;
    fprintf(stderr, "          (total: %d/%d, loss=%.2f%%, client_recv=%d, routing: %s)\n",
            server_recv, total_sent, loss, total_client_recv,
            routing_ok ? "OK" : "FAIL");

    ASSERT(server_recv >= total_sent * 99 / 100);
    ASSERT(routing_ok);

    stop_proxy(proxy);
    for (int c = 0; c < num_clients; c++) close(client_fds[c]);
    close(server_fd);
}

/* ---- Scenario 4: Server-initiated rekey ---- */

static void test_server_rekey(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("server", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Client sends AWG init to establish session */
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
    make_awg_init(awg_init, 0x5000);
    sendto(client_fd, awg_init, sizeof(awg_init), 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    /* Get proxy's address from server side */
    struct sockaddr_in proxy_remote_addr;
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < TEST_JC + 5; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) {
                proxy_remote_addr = from;
                got = 1;
            }
        }
        ASSERT(got);
        while (recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT, NULL, NULL) > 0);
    }

    /* Server sends WG handshake init (server-initiated rekey) */
    uint8_t init[WG_INIT_SIZE];
    make_wg_init(init, 0x6000);
    sendto(server_fd, init, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));

    /* Client should receive it (proxy transforms WG->AWG and routes to sole client) */
    usleep(300000);
    uint8_t rbuf[2048];
    int delivered = 0;
    for (int i = 0; i < 10; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 500);
        if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) { delivered = 1; break; }
        }
    }

    fprintf(stderr, "          (delivered: %d/1)\n", delivered);
    ASSERT(delivered);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 5: Concurrent handshakes ---- */

typedef struct {
    int fd;
    struct sockaddr_in dest;
    uint32_t sender_index;
} hs_thread_args_t;

static void *hs_sender(void *arg) {
    hs_thread_args_t *a = (hs_thread_args_t *)arg;
    uint8_t init[WG_INIT_SIZE];
    make_wg_init(init, a->sender_index);
    sendto(a->fd, init, WG_INIT_SIZE, 0,
           (struct sockaddr *)&a->dest, sizeof(a->dest));
    return NULL;
}

static void test_concurrent_handshakes(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    struct sockaddr_in proxy_addr = make_addr(listen_port);
    int nthreads = 8;
    pthread_t threads[8];
    hs_thread_args_t args[8];

    /* Each thread gets its own socket to avoid contention */
    for (int i = 0; i < nthreads; i++) {
        args[i].fd = make_client_socket();
        ASSERT(args[i].fd >= 0);
        args[i].dest = proxy_addr;
        args[i].sender_index = 0xA000 + (uint32_t)i;
    }

    /* Launch all threads simultaneously */
    for (int i = 0; i < nthreads; i++)
        pthread_create(&threads[i], NULL, hs_sender, &args[i]);
    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    /* Server receives: each init produces junk + transformed init.
     * Total expected: nthreads inits (+ junk).
     * Verify each init is valid (correct size, H1 type, no corruption). */
    usleep(500000);
    int valid = 0, corrupted = 0;
    uint8_t rbuf[2048];
    for (int i = 0; i < nthreads * (TEST_JC + 2); i++) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(server_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
        if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) {
                /* Verify sender_index is one of ours */
                uint32_t si;
                memcpy(&si, rbuf + TEST_S1 + 4, 4);
                int found = 0;
                for (int j = 0; j < nthreads; j++) {
                    if (si == args[j].sender_index) { found = 1; break; }
                }
                if (found) valid++;
                else corrupted++;
            }
        }
    }

    fprintf(stderr, "          (valid: %d/%d, corrupted: %d)\n", valid, nthreads, corrupted);
    ASSERT(valid == nthreads);
    ASSERT(corrupted == 0);

    for (int i = 0; i < nthreads; i++) close(args[i].fd);
    stop_proxy(proxy);
    close(server_fd);
}

/* ---- Scenario 5b: two handshakes for different peers in one batch ----
 *
 * In server/reverse mode the s2c headroom is only max_s4, so any handshake with
 * S1/S2/S3 above that is built in the transform's shared buffer. Queueing such a
 * packet in the sendmmsg batch and then transforming the next one overwrites it
 * before the batch is sent: with two peers rekeying at the same moment each gets
 * the other's packet and its own handshake is lost. Here S2=15 and S4=0, so
 * every response takes that path. */
static void test_server_handshake_batch(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("server", listen_port, remote_port);
    ASSERT(proxy > 0);

    struct sockaddr_in proxy_addr = make_addr(listen_port);
    const uint32_t idx_a = 0x7100, idx_b = 0x7200;
    int fd_a = make_client_socket();
    int fd_b = make_client_socket();
    ASSERT(fd_a >= 0 && fd_b >= 0);

    /* Both clients establish a session so the proxy can route responses. */
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];
    make_awg_init(awg_init, idx_a);
    sendto(fd_a, awg_init, sizeof(awg_init), 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    make_awg_init(awg_init, idx_b);
    sendto(fd_b, awg_init, sizeof(awg_init), 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    /* Learn the proxy's source address as the server sees it, then drain. */
    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < 2 * (TEST_JC + 2); i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { proxy_remote_addr = from; got = 1; }
        }
        ASSERT(got);
    }

    /* Rounds of two back-to-back responses, one per peer. Sending them without a
     * gap is what puts both in the same recvmmsg batch; several rounds cover the
     * case where the kernel splits a pair. */
    const int rounds = 25;
    int delivered_a = 0, delivered_b = 0, crossed = 0;

    for (int r = 0; r < rounds; r++) {
        uint32_t mark_a = 0xA0000000u + (uint32_t)r;
        uint32_t mark_b = 0xB0000000u + (uint32_t)r;
        uint8_t resp[WG_RESP_SIZE];

        make_wg_response(resp, mark_a, idx_a);
        sendto(server_fd, resp, sizeof(resp), 0,
               (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
        make_wg_response(resp, mark_b, idx_b);
        sendto(server_fd, resp, sizeof(resp), 0,
               (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));

        /* Each client must get its own response: same size, H2 type, and the
         * marker this round put in that peer's packet. */
        struct { int fd; uint32_t mine, theirs; int *hit; } peers[2] = {
            { fd_a, mark_a, mark_b, &delivered_a },
            { fd_b, mark_b, mark_a, &delivered_b },
        };
        for (int p = 0; p < 2; p++) {
            uint8_t rbuf[2048];
            for (int attempt = 0; attempt < 4; attempt++) {
                int n = recv_one(peers[p].fd, rbuf, sizeof(rbuf), 300);
                if (n != TEST_S2 + WG_RESP_SIZE) continue;
                uint32_t h, mark;
                memcpy(&h, rbuf + TEST_S2, 4);
                if (h != TEST_H2) continue;
                memcpy(&mark, rbuf + TEST_S2 + 4, 4);
                if (mark == peers[p].mine) (*peers[p].hit)++;
                else if (mark == peers[p].theirs) crossed++;
                break;
            }
        }
    }

    fprintf(stderr, "          (own: %d+%d/%d, crossed: %d)\n",
            delivered_a, delivered_b, 2 * rounds, crossed);

    ASSERT_EQ(crossed, 0);
    ASSERT(delivered_a + delivered_b >= 2 * rounds * 9 / 10);

    stop_proxy(proxy);
    close(fd_a);
    close(fd_b);
    close(server_fd);
}

/* ---- Scenario 6: Scale test — 100K / 1M / 10M through one proxy ---- */

static void test_scale(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Warm up: handshake to establish client address in proxy */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0xF000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);
    drain_socket(server_fd);

    long base_rss = get_rss_kb(proxy);
    int scales[] = { 100000, 1000000, 5000000 };
    const char *labels[] = { "100K", "1M", "5M" };
    double send_rates[3] = {0};

    /* Pre-fill transport packet (proxy doesn't inspect payload beyond type) */
    uint8_t transport[200];
    make_wg_transport(transport, 0x2000, 0, 200);

    for (int s = 0; s < 3; s++) {
        int count = scales[s];
        long rss_before = get_rss_kb(proxy);

        async_recv_t srv_recv;
        pthread_t srv_thread;
        async_recv_start(&srv_recv, &srv_thread, server_fd);

        int64_t t0 = now_us();

        for (int i = 0; i < count; i++) {
            sendto(client_fd, transport, 200, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            if ((i + 1) % 64 == 0) usleep(100);
        }

        int64_t t_sent = now_us();

        /* Wait for proxy to flush — scale with packet count */
        int wait_us = count >= 2500000 ? 2000000
                    : count >= 500000  ? 1500000
                    :                     500000;
        usleep(wait_us);

        int received = async_recv_stop(&srv_recv, srv_thread);
        drain_socket(server_fd); /* clean slate for next round */
        long rss_after = get_rss_kb(proxy);

        double send_s = (t_sent - t0) / 1e6;
        double pps = count / send_s;
        double mbps = (count * 200.0) / send_s / (1024 * 1024);
        send_rates[s] = pps;
        double loss = 100.0 * (count - received) / count;
        long rss_delta = rss_after - rss_before;

        fprintf(stderr, "          %3s: %d/%d loss=%.2f%%  "
                "%.1fK pkt/s (%.0f MB/s)  RSS: %ld→%ldKB (Δ%+ldKB)\n",
                labels[s], received, count, loss,
                pps / 1000, mbps, rss_before, rss_after, rss_delta);

        ASSERT(received >= count * 99 / 100); /* < 1% loss */
        ASSERT(rss_delta < 1024);             /* no leak: < 1MB growth */
    }

    /* Throughput consistency: 5M rate should be >= 80% of 100K rate */
    if (send_rates[0] > 0) {
        double ratio = send_rates[2] / send_rates[0];
        fprintf(stderr, "          throughput 5M/100K: %.0f%%\n", ratio * 100);
        ASSERT(ratio >= 0.80);
    }

    long final_rss = get_rss_kb(proxy);
    fprintf(stderr, "          memory: base=%ldKB final=%ldKB (Δ%+ldKB)\n",
            base_rss, final_rss, final_rss - base_rss);
    ASSERT(final_rss - base_rss < 1024);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 7: GSO on connected socket ---- */

#ifndef UDP_SEGMENT
#define UDP_SEGMENT 103
#endif

static void test_gso_connected(void) {
    /* Verify kernel UDP GSO works with msg_name=NULL (connected socket).
     * Before the fix, send_gso() returned early when addr==NULL,
     * preventing GSO on the upload (c2s) path. */
    int recv_port = find_free_port();
    ASSERT(recv_port > 0);

    int recv_fd = make_udp_socket(recv_port);
    ASSERT(recv_fd >= 0);

    int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT(send_fd >= 0);
    struct sockaddr_in dest = make_addr(recv_port);
    int ret = connect(send_fd, (struct sockaddr *)&dest, sizeof(dest));
    ASSERT(ret == 0);

    /* Build 4 same-size packets in one buffer */
    int seg_size = 200;
    int count = 4;
    uint8_t data[800];
    for (int i = 0; i < count; i++)
        memset(data + i * seg_size, (uint8_t)(i + 1), seg_size);

    /* sendmsg with UDP_SEGMENT cmsg, msg_name=NULL */
    struct iovec iov = { .iov_base = data, .iov_len = (size_t)(seg_size * count) };

    union {
        char buf[CMSG_SPACE(sizeof(uint16_t))];
        struct cmsghdr align;
    } cmsg_u;
    memset(&cmsg_u, 0, sizeof(cmsg_u));

    struct msghdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.msg_iov = &iov;
    hdr.msg_iovlen = 1;
    hdr.msg_control = cmsg_u.buf;
    hdr.msg_controllen = sizeof(cmsg_u.buf);
    /* msg_name = NULL — connected socket, this is the scenario the fix enables */

    struct cmsghdr *cm = CMSG_FIRSTHDR(&hdr);
    cm->cmsg_level = IPPROTO_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    uint16_t ss = (uint16_t)seg_size;
    memcpy(CMSG_DATA(cm), &ss, sizeof(ss));

    ssize_t sent = sendmsg(send_fd, &hdr, 0);
    if (sent < 0 && errno == ENOPROTOOPT) {
        fprintf(stderr, "          (kernel lacks UDP_SEGMENT, skipping)\n");
        close(send_fd);
        close(recv_fd);
        return;
    }
    ASSERT_EQ((int)sent, seg_size * count);

    /* Verify all 4 packets arrived individually */
    usleep(50000);
    int received = 0;
    uint8_t rbuf[256];
    for (int i = 0; i < count + 2; i++) {
        ssize_t n = recvfrom(recv_fd, rbuf, sizeof(rbuf), MSG_DONTWAIT, NULL, NULL);
        if (n <= 0) break;
        ASSERT_EQ((int)n, seg_size);
        ASSERT_EQ(rbuf[0], (uint8_t)(received + 1));
        received++;
    }
    ASSERT_EQ(received, count);

    fprintf(stderr, "          (GSO connected: sent %d segments, received %d)\n", count, received);
    close(send_fd);
    close(recv_fd);
}

/* ---- Scenario 8: GRO-enabled bidirectional throughput ---- */

static void test_gro_bidirectional(void) {
    /* Run proxy WITH GRO enabled and verify both directions work.
     * Before the fix: GRO was only on s2c (download).
     * After: GRO on both directions, GSO on both directions. */
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_with_gro("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0xD000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    /* Capture proxy's remote address from server side */
    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < TEST_JC + 5; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { proxy_remote_addr = from; got = 1; }
        }
        ASSERT(got);
        drain_socket(server_fd);
    }

    int count = BIDIR_COUNT;

    /* c2s (upload): client → proxy → server */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    int64_t t0_c2s = now_us();
    {
        uint8_t transport[200];
        for (int i = 0; i < count; i++) {
            make_wg_transport(transport, 0x2000, (uint64_t)i, 200);
            sendto(client_fd, transport, 200, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            if ((i + 1) % 64 == 0) usleep(100);
        }
    }
    int64_t t1_c2s = now_us();

    usleep(500000);
    int c2s_recv = async_recv_stop(&srv_recv, srv_thread);
    drain_socket(server_fd);

    /* s2c (download): server → proxy → client */
    drain_socket(client_fd);
    async_recv_t cli_recv;
    pthread_t cli_thread;
    async_recv_start(&cli_recv, &cli_thread, client_fd);

    int64_t t0_s2c = now_us();
    {
        uint8_t transport[200];
        for (int i = 0; i < count; i++) {
            make_awg_transport(transport, 0x2000, (uint64_t)i, 200);
            sendto(server_fd, transport, 200, 0,
                   (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
            if ((i + 1) % 64 == 0) usleep(100);
        }
    }
    int64_t t1_s2c = now_us();

    usleep(500000);
    int s2c_recv = async_recv_stop(&cli_recv, cli_thread);

    double c2s_time = (t1_c2s - t0_c2s) / 1e6;
    double s2c_time = (t1_s2c - t0_s2c) / 1e6;
    double c2s_loss = 100.0 * (count - c2s_recv) / count;
    double s2c_loss = 100.0 * (count - s2c_recv) / count;

    fprintf(stderr, "          c2s: %d/%d (%.2f%% loss, %.3fs)  "
            "s2c: %d/%d (%.2f%% loss, %.3fs)\n",
            c2s_recv, count, c2s_loss, c2s_time,
            s2c_recv, count, s2c_loss, s2c_time);

    ASSERT(c2s_recv >= count * 99 / 100);
    ASSERT(s2c_recv >= count * 99 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 9: Throughput benchmark (realistic MTU) ---- */

static void test_throughput_benchmark(void) {
    /* Measure real throughput in both directions with realistic WG packet size.
     * WG transport overhead: 32 bytes header + 16 bytes AEAD tag = 1432 bytes for MTU 1420.
     * Plus AWG overhead: S1 padding on init, H4 type swap on transport. */
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_with_gro("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0xE000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    usleep(300000);

    struct sockaddr_in proxy_remote_addr;
    memset(&proxy_remote_addr, 0, sizeof(proxy_remote_addr));
    {
        uint8_t tmp[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got = 0;
        for (int i = 0; i < TEST_JC + 5; i++) {
            ssize_t n = recvfrom(server_fd, tmp, sizeof(tmp), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &fromlen);
            if (n > 0) { proxy_remote_addr = from; got = 1; }
        }
        ASSERT(got);
        drain_socket(server_fd);
    }

    /* Realistic WG transport packet: 1432 bytes (MTU 1420 - IP/UDP overhead absorbed) */
    int pkt_size = 1400;
    int duration_pkts = 500000;

    /* Pre-fill packet */
    uint8_t wg_pkt[1500];
    make_wg_transport(wg_pkt, 0x2000, 0, pkt_size);
    uint8_t awg_pkt[1500];
    make_awg_transport(awg_pkt, 0x2000, 0, pkt_size);

    /* c2s benchmark (upload) */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    int64_t t0 = now_us();
    for (int i = 0; i < duration_pkts; i++) {
        sendto(client_fd, wg_pkt, pkt_size, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(50);
    }
    int64_t t1 = now_us();

    usleep(1000000);
    int c2s_recv = async_recv_stop(&srv_recv, srv_thread);
    double c2s_sec = (t1 - t0) / 1e6;
    double c2s_mbps = (double)c2s_recv * pkt_size * 8.0 / c2s_sec / 1e6;

    /* s2c benchmark (download) */
    drain_socket(client_fd);
    drain_socket(server_fd);

    async_recv_t cli_recv;
    pthread_t cli_thread;
    async_recv_start(&cli_recv, &cli_thread, client_fd);

    int64_t t2 = now_us();
    for (int i = 0; i < duration_pkts; i++) {
        sendto(server_fd, awg_pkt, pkt_size, 0,
               (struct sockaddr *)&proxy_remote_addr, sizeof(proxy_remote_addr));
        if ((i + 1) % 64 == 0) usleep(50);
    }
    int64_t t3 = now_us();

    usleep(1000000);
    int s2c_recv = async_recv_stop(&cli_recv, cli_thread);
    double s2c_sec = (t3 - t2) / 1e6;
    double s2c_mbps = (double)s2c_recv * pkt_size * 8.0 / s2c_sec / 1e6;

    double c2s_loss = 100.0 * (duration_pkts - c2s_recv) / duration_pkts;
    double s2c_loss = 100.0 * (duration_pkts - s2c_recv) / duration_pkts;

    fprintf(stderr, "          packet size: %d bytes,  count: %d\n", pkt_size, duration_pkts);
    fprintf(stderr, "          c2s(upload):   %7.1f Mbit/s  (%d/%d, loss=%.2f%%)\n",
            c2s_mbps, c2s_recv, duration_pkts, c2s_loss);
    fprintf(stderr, "          s2c(download): %7.1f Mbit/s  (%d/%d, loss=%.2f%%)\n",
            s2c_mbps, s2c_recv, duration_pkts, s2c_loss);

    double ratio = (c2s_mbps > s2c_mbps) ? s2c_mbps / c2s_mbps : c2s_mbps / s2c_mbps;
    fprintf(stderr, "          parity: %.0f%%\n", ratio * 100);

    ASSERT(c2s_recv >= duration_pkts * 98 / 100);
    ASSERT(s2c_recv >= duration_pkts * 98 / 100);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 10: Site-to-site profile fallback (initiator) ---- */

/* Primary (v2) profile — deliberately distinct sizes/types from the v1
 * fallback so the mock server can tell which profile obfuscated each init. */
#define FB_PRI_S1 25
#define FB_PRI_S2 15
#define FB_PRI_H1 2000000001u
#define FB_PRI_H2 2000000002u
#define FB_PRI_H3 2000000003u
#define FB_PRI_H4 2000000004u

static pid_t start_proxy_fallback(int listen_port, int remote_port) {
    int src_port = find_free_port();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char lbuf[32], rbuf[64], sp[8];
        char jc[8], jmin[8], jmax[8];
        char s1[8], s2[8], h1[16], h2[16], h3[16], h4[16];
        char fs1[8], fs2[8], fh1[16], fh2[16], fh3[16], fh4[16];

        snprintf(lbuf, sizeof(lbuf), ":%d", listen_port);
        snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);

        setenv("AWG_LISTEN", lbuf, 1);
        setenv("AWG_REMOTE", rbuf, 1);
        setenv("AWG_MODE", "normal", 1);
        setenv("AWG_JC", itoa_buf(TEST_JC, jc), 1);
        setenv("AWG_JMIN", itoa_buf(TEST_JMIN, jmin), 1);
        setenv("AWG_JMAX", itoa_buf(TEST_JMAX, jmax), 1);
        /* Primary profile */
        setenv("AWG_S1", itoa_buf(FB_PRI_S1, s1), 1);
        setenv("AWG_S2", itoa_buf(FB_PRI_S2, s2), 1);
        setenv("AWG_H1", utoa_buf(FB_PRI_H1, h1), 1);
        setenv("AWG_H2", utoa_buf(FB_PRI_H2, h2), 1);
        setenv("AWG_H3", utoa_buf(FB_PRI_H3, h3), 1);
        setenv("AWG_H4", utoa_buf(FB_PRI_H4, h4), 1);
        /* v1 fallback profile (reuses the TEST_* constants) */
        setenv("AWG_FB_S1", itoa_buf(TEST_S1, fs1), 1);
        setenv("AWG_FB_S2", itoa_buf(TEST_S2, fs2), 1);
        setenv("AWG_FB_H1", utoa_buf(TEST_H1, fh1), 1);
        setenv("AWG_FB_H2", utoa_buf(TEST_H2, fh2), 1);
        setenv("AWG_FB_H3", utoa_buf(TEST_H3, fh3), 1);
        setenv("AWG_FB_H4", utoa_buf(TEST_H4, fh4), 1);
        setenv("AWG_FB_AFTER", "5", 1);
        setenv("AWG_SERVER_PUB", DUMMY_SERVER_PUB, 1);
        setenv("AWG_CLIENT_PUB", DUMMY_CLIENT_PUB, 1);
        setenv("AWG_LOG_LEVEL", "error", 1);
        setenv("AWG_TIMEOUT", "60", 1);
        setenv("AWG_NO_GRO", "1", 1);
        setenv("AWG_SRC_PORT", itoa_buf(src_port, sp), 1);

        execl(PROXY_BINARY, "awg-proxy", NULL);
        _exit(127);
    }
    usleep(200000);
    return pid;
}

static void test_s2s_fallback(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_fallback(listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Simulate WireGuard retransmitting handshake inits while the remote stays
     * silent (as an old v1 peer would when it can't decode the v2 obfuscation).
     * Primary (v2) inits must appear first; after fb_after seconds of silence
     * the proxy must switch to the v1 fallback profile. */
    int primary_seen = 0, fallback_seen = 0;
    int primary_first = 0, fallback_after_primary = 0;
    uint8_t rbuf[2048];
    int64_t start = now_us();
    int64_t last_send = 0;
    uint32_t si = 0x7000;

    while ((now_us() - start) < 12000000) { /* run for 12s */
        int64_t nowt = now_us();
        if (nowt - last_send >= 500000) { /* resend init every 500ms */
            uint8_t init_buf[WG_INIT_SIZE];
            make_wg_init(init_buf, si++);
            sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
                   (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
            last_send = nowt;
        }
        int n = recv_one(server_fd, rbuf, sizeof(rbuf), 100);
        if (n == FB_PRI_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + FB_PRI_S1, 4);
            if (h == FB_PRI_H1) {
                primary_seen++;
                if (!fallback_seen) primary_first = 1;
            }
        } else if (n == TEST_S1 + WG_INIT_SIZE) {
            uint32_t h;
            memcpy(&h, rbuf + TEST_S1, 4);
            if (h == TEST_H1) {
                fallback_seen++;
                if (primary_seen) fallback_after_primary = 1;
            }
        }
    }

    fprintf(stderr, "          (primary inits=%d, fallback inits=%d, order primary->fallback=%s)\n",
            primary_seen, fallback_seen,
            (primary_first && fallback_after_primary) ? "yes" : "no");
    ASSERT(primary_seen > 0);       /* started on the primary profile */
    ASSERT(fallback_seen > 0);      /* switched to fallback after silence */
    ASSERT(fallback_after_primary); /* primary was tried before the fallback */

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Main ---- */

/* ---- Scenario 11: AWG 3.0 header protection end to end ---- */

/* Recover the message type of a received v3 datagram: nonce is the first 12
 * bytes, the type is the first 4 keystream bytes XOR'd into the message. */
static uint32_t v3_unmask_type(const uint8_t *pkt, int pad) {
    uint8_t ks[CHACHA20_BLOCK_SIZE];
    uint32_t onwire, mask;
    chacha20_block(TEST_HP_KEY, pkt, 0, ks);
    memcpy(&onwire, pkt + pad, 4);
    memcpy(&mask, ks, 4);
    return onwire ^ mask;
}

/* Build what a real AWG 3.0 peer would put on the wire for a transport packet. */
static int make_awg_v3_transport(uint8_t *buf, uint32_t receiver_index,
                                 uint64_t counter, int msg_size) {
    for (int i = 0; i < TEST_V3_S4; i++)
        buf[i] = (uint8_t)(counter * 31 + i * 7 + 3);
    uint8_t *msg = buf + TEST_V3_S4;
    memset(msg, 0, (size_t)msg_size);
    uint32_t t = TEST_H4;
    memcpy(msg, &t, 4);
    memcpy(msg + 4, &receiver_index, 4);
    memcpy(msg + 8, &counter, 8);
    for (int i = 16; i < msg_size; i++)
        msg[i] = (uint8_t)(i ^ (counter & 0xFF));
    chacha20_xor(TEST_HP_KEY, buf, msg, AWG_HP_TRANSPORT_HDR);
    return TEST_V3_S4 + msg_size;
}

/* AWG 3.1 handshake as a real peer would send it: S padding, the whole message
 * encrypted under the header key, then a random tail left in the clear. */
static int make_awg_v31_handshake(uint8_t *buf, uint32_t htype, int pad,
                                  int msg_size, uint32_t sender_index,
                                  int trailer) {
    for (int i = 0; i < pad; i++)
        buf[i] = (uint8_t)(sender_index * 13 + i * 5 + 1);
    uint8_t *msg = buf + pad;
    memset(msg, 0, (size_t)msg_size);
    memcpy(msg, &htype, 4);
    memcpy(msg + 4, &sender_index, 4);
    for (int i = 8; i < msg_size; i++)
        msg[i] = (uint8_t)(i ^ (sender_index & 0xFF));
    chacha20_xor(TEST_HP_KEY, buf, msg, msg_size);
    for (int i = 0; i < trailer; i++)
        buf[pad + msg_size + i] = (uint8_t)(i * 29 + 7);
    return pad + msg_size + trailer;
}

static void make_wg_cookie(uint8_t *buf, uint32_t receiver_index) {
    memset(buf, 0, WG_COOKIE_SIZE);
    uint32_t t = WG_COOKIE_REPLY;
    memcpy(buf, &t, 4);
    memcpy(buf + 4, &receiver_index, 4);
    for (int i = 8; i < WG_COOKIE_SIZE; i++)
        buf[i] = (uint8_t)(i * 3 + 1);
}

/* AWG 3.1: outgoing handshakes carry a random tail, incoming ones are accepted
 * with any tail, and cookie replies never leave. */
static void test_v31_random_trailers(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_v31("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* c2s: several inits, each of which must arrive at or above the plain 3.0
     * size, inside the default 500-byte window — and not always at the minimum,
     * or nothing is being randomised at all. */
    const int min_len = TEST_V3_S1 + WG_INIT_SIZE;
    int seen = 0, padded = 0, longest = 0;
    struct sockaddr_in from = {0};
    socklen_t fromlen = sizeof(from);
    uint8_t rbuf[2048];

    for (int round = 0; round < 8; round++) {
        uint8_t init_buf[WG_INIT_SIZE];
        make_wg_init(init_buf, 0x3100u + (uint32_t)round);
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

        for (int i = 0; i < TEST_JC + 5; i++) {
            struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
            if (poll(&pfd, 1, 500) <= 0) break;
            fromlen = sizeof(from);
            int n = (int)recvfrom(server_fd, rbuf, sizeof(rbuf), 0,
                                  (struct sockaddr *)&from, &fromlen);
            if (n < min_len) continue;
            if (v3_unmask_type(rbuf, TEST_V3_S1) != TEST_H1) continue;
            ASSERT(n <= AWG_DEFAULT_UDP_WINDOW);
            seen++;
            if (n > min_len) padded++;
            if (n > longest) longest = n;
            break;
        }
    }
    fprintf(stderr, "          (inits=%d, padded=%d, longest=%d)\n",
            seen, padded, longest);
    ASSERT(seen >= 6);
    ASSERT(padded > 0);
    drain_socket(server_fd);

    /* s2c: a response with a tail must reach the client as a plain 92-byte WG
     * response, tail cut off. */
    uint8_t awg[1500];
    int awg_len = make_awg_v31_handshake(awg, TEST_H2, TEST_V3_S2, WG_RESP_SIZE,
                                         0x3200, 137);
    sendto(server_fd, awg, (size_t)awg_len, 0, (struct sockaddr *)&from, fromlen);

    int got_resp = 0;
    for (int i = 0; i < 5; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 1000);
        if (n != WG_RESP_SIZE) continue;
        uint32_t t;
        memcpy(&t, rbuf, 4);
        if (t != WG_HANDSHAKE_RESPONSE) continue;
        uint32_t sidx;
        memcpy(&sidx, rbuf + 4, 4);
        ASSERT_EQ(sidx, 0x3200u);
        got_resp = 1;
        break;
    }
    ASSERT(got_resp);

    /* Cookie replies are disabled: nothing at all may reach the server. */
    uint8_t cookie[WG_COOKIE_SIZE];
    make_wg_cookie(cookie, 0x3200);
    sendto(client_fd, cookie, WG_COOKIE_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
    ASSERT(poll(&pfd, 1, 500) == 0);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

static void test_v3_header_protection(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    pid_t proxy = start_proxy_v3("normal", listen_port, remote_port);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* c2s handshake: the init must arrive padded, and its type must only be
     * readable after ChaCha20 — not in the clear. */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x3000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    usleep(200000);
    uint8_t rbuf[2048];
    int init_received = 0;
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    for (int i = 0; i < TEST_JC + 5; i++) {
        struct pollfd pfd = { .fd = server_fd, .events = POLLIN };
        if (poll(&pfd, 1, 500) <= 0) break;
        fromlen = sizeof(from);
        int n = (int)recvfrom(server_fd, rbuf, sizeof(rbuf), 0,
                              (struct sockaddr *)&from, &fromlen);
        if (n != TEST_V3_S1 + WG_INIT_SIZE) continue;
        uint32_t clear;
        memcpy(&clear, rbuf + TEST_V3_S1, 4);
        ASSERT(clear != TEST_H1); /* header must be encrypted on the wire */
        if (v3_unmask_type(rbuf, TEST_V3_S1) == TEST_H1) { init_received = 1; break; }
    }
    ASSERT(init_received);
    drain_socket(server_fd);

    /* c2s transport: burst, check loss and that every packet is protected */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    uint8_t transport[200];
    const int v3_burst = BURST_COUNT / 2;
    for (int i = 0; i < v3_burst; i++) {
        make_wg_transport(transport, 0x4000, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(100);
    }
    usleep(500000);
    int received = async_recv_stop(&srv_recv, srv_thread);
    double loss = 100.0 * (v3_burst - received) / v3_burst;
    fprintf(stderr, "          (c2s sent=%d, recv=%d, loss=%.2f%%)\n",
            v3_burst, received, loss);
    ASSERT(received >= v3_burst * 99 / 100);
    drain_socket(server_fd);

    /* s2c: a genuine AWG 3.0 transport packet must come back out as plain WG */
    uint8_t awg[TEST_V3_S4 + 200];
    int awg_len = make_awg_v3_transport(awg, 0x3000, 77, 200);
    sendto(server_fd, awg, (size_t)awg_len, 0, (struct sockaddr *)&from, fromlen);

    int got_wg = 0;
    for (int i = 0; i < 5; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 1000);
        if (n != 200) continue;
        uint32_t t;
        memcpy(&t, rbuf, 4);
        if (t != WG_TRANSPORT_DATA) continue;
        uint32_t ridx;
        uint64_t ctr;
        memcpy(&ridx, rbuf + 4, 4);
        memcpy(&ctr, rbuf + 8, 8);
        ASSERT_EQ(ridx, 0x3000u);
        ASSERT_EQ(ctr, 77u);
        for (int j = 16; j < 200; j++)
            ASSERT_EQ(rbuf[j], (uint8_t)(j ^ 77));
        got_wg = 1;
        break;
    }
    ASSERT(got_wg);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 12: IPv6 remote leg ---- */

/* Wait for the transformed handshake init on an already-bound mock server.
 * Junk and CPS packets precede it, so the size + H1 pair is the marker. */
static int await_awg_init(int fd, struct sockaddr_storage *from, socklen_t *fromlen,
                          int tries, int timeout_ms) {
    uint8_t rbuf[2048];
    for (int i = 0; i < tries; i++) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, timeout_ms) <= 0) break;
        *fromlen = sizeof(*from);
        int n = (int)recvfrom(fd, rbuf, sizeof(rbuf), 0,
                              (struct sockaddr *)from, fromlen);
        if (n != TEST_S1 + WG_INIT_SIZE) continue;
        uint32_t h;
        memcpy(&h, rbuf + TEST_S1, 4);
        if (h == TEST_H1) return 1;
    }
    return 0;
}

static void test_ipv6_remote(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int server_fd = make_udp_socket6(remote_port);
    if (server_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "[::1]:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, NULL);
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake over the v6 leg */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x6000);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    int init_received = await_awg_init(server_fd, &from, &fromlen, TEST_JC + 5, 1000);
    ASSERT(init_received);
    ASSERT_EQ(from.ss_family, AF_INET6);
    drain_socket(server_fd);

    /* c2s transport over IPv6 */
    async_recv_t srv_recv;
    pthread_t srv_thread;
    async_recv_start(&srv_recv, &srv_thread, server_fd);

    const int burst = BURST_COUNT / 4;
    uint8_t transport[200];
    for (int i = 0; i < burst; i++) {
        make_wg_transport(transport, 0x6001, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        if ((i + 1) % 64 == 0) usleep(100);
    }
    usleep(500000);
    int received = async_recv_stop(&srv_recv, srv_thread);
    fprintf(stderr, "          (c2s over IPv6: sent=%d, recv=%d)\n", burst, received);
    ASSERT(received >= burst * 99 / 100);

    /* s2c: an AWG transport packet from the v6 server must reach the client */
    uint8_t awg[200];
    make_awg_transport(awg, 0x6000, 4242, 200);
    sendto(server_fd, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);

    uint8_t rbuf[2048];
    int got_wg = 0;
    for (int i = 0; i < 5; i++) {
        int n = recv_one(client_fd, rbuf, sizeof(rbuf), 1000);
        if (n != 200) continue;
        uint32_t t;
        memcpy(&t, rbuf, 4);
        if (t != WG_TRANSPORT_DATA) continue;
        uint64_t ctr;
        memcpy(&ctr, rbuf + 8, 8);
        ASSERT_EQ(ctr, 4242u);
        got_wg = 1;
        break;
    }
    ASSERT(got_wg);

    stop_proxy(proxy);
    close(client_fd);
    close(server_fd);
}

/* ---- Scenario 13: Happy Eyeballs — IPv4 black hole, IPv6 answers ---- */

static void test_happy_eyeballs(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    /* "localhost" carries both an A and an AAAA record in every stock
     * /etc/hosts, which is exactly the dual-stack name the probe is for. */
    int v6_fd = make_udp_socket6(remote_port);
    if (v6_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    /* Bound but never answering: a real black hole rather than an ICMP reject,
     * so the probe has to reach its timeout instead of an error. */
    int v4_blackhole = make_udp_socket(remote_port);
    ASSERT(v4_blackhole >= 0);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "200");
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x7100);
    int64_t t0 = now_us();
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    /* The replayed init must show up on the IPv6 socket well inside the
     * handshake retry interval — the point of the probe is not to wait for a
     * timeout. Allow generous slack for a loaded CI box. */
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    int init_received = await_awg_init(v6_fd, &from, &fromlen, TEST_JC + 5, 2000);
    int64_t elapsed_ms = (now_us() - t0) / 1000;
    fprintf(stderr, "          (IPv6 init after %lldms)\n", (long long)elapsed_ms);
    ASSERT(init_received);
    ASSERT_EQ(from.ss_family, AF_INET6);
    /* Must track AWG_HE_DELAY, not some poll tick that happens to be close.
     * WireGuard's own retry is 5s away, so a regression to "wait for the next
     * retransmit" would sail past this bound. */
    ASSERT(elapsed_ms < 1000);

    /* Answer over IPv6 so the probe locks onto that family... */
    uint8_t awg[200];
    make_awg_transport(awg, 0x7100, 1, 200);
    sendto(v6_fd, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);
    usleep(300000);
    drain_socket(v6_fd);
    drain_socket(v4_blackhole);

    /* ...and everything after the switch must go there, not to the v4 hole. */
    uint8_t transport[200];
    for (int i = 0; i < 200; i++) {
        make_wg_transport(transport, 0x7101, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    }
    usleep(400000);

    int on_v6 = 0, on_v4 = 0;
    uint8_t buf[2048];
    while (recvfrom(v6_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) on_v6++;
    while (recvfrom(v4_blackhole, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) on_v4++;
    fprintf(stderr, "          (after switch: v6=%d, v4=%d)\n", on_v6, on_v4);
    ASSERT(on_v6 >= 200 * 99 / 100);
    ASSERT_EQ(on_v4, 0);

    stop_proxy(proxy);
    close(client_fd);
    close(v6_fd);
    close(v4_blackhole);
}

/* ---- Scenario 14: Happy Eyeballs — a server that is merely down must not
 *      cost the tunnel its family ----
 *
 * Replays the outage this test was written for: the hub was restarted while the
 * client was reconnecting, so both families went quiet at once. Silence from
 * both proves nothing about either path, yet the probe used to hand the run to
 * the alternate socket on its deadline and record that as the new dial order.
 * When the hub came back seconds later the client was pinned to a family that
 * never worked on that route, and it stayed pinned across reconnects.
 *
 * Both sockets are bound black holes, so the probe has to reach its 15 s
 * deadline. Everything after it must still leave on the primary, and the tunnel
 * must come up the moment the primary starts answering. */
static void test_he_both_silent_keeps_primary(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int v6_fd = make_udp_socket6(remote_port);
    if (v6_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    /* Bound, never answering — a black hole rather than an ICMP reject, so the
     * probe times out instead of taking the POLLERR shortcut. */
    int v4_fd = make_udp_socket(remote_port);
    ASSERT(v4_fd >= 0);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "200");
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* With no state file the dial order is IPv4 first, IPv6 as the alternate. */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x7200);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    /* The probe must actually be running, or the rest proves nothing. */
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ASSERT(await_awg_init(v6_fd, &from, &fromlen, TEST_JC + 5, 2000));
    ASSERT_EQ(from.ss_family, AF_INET6);

    /* Sit out the whole deadline with both families mute. */
    fprintf(stderr, "          (waiting out the 15s probe deadline...)\n");
    sleep(17);
    drain_socket(v4_fd);
    drain_socket(v6_fd);

    /* The primary is the only thing the proxy learned nothing against, so it
     * must still be carrying the traffic. */
    uint8_t transport[200];
    for (int i = 0; i < 50; i++) {
        make_wg_transport(transport, 0x7201, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    }
    usleep(400000);

    /* Transport is what the tunnel carries; the probe's replayed handshake
     * init is a different size and keeps arriving on the alt on purpose, so
     * the two have to be counted apart. */
    int on_v4 = 0, v6_transport = 0, v6_probe = 0;
    uint8_t buf[2048];
    struct sockaddr_storage v4_peer;
    socklen_t v4_peerlen = sizeof(v4_peer);
    ssize_t n;
    while ((n = recvfrom(v4_fd, buf, sizeof(buf), MSG_DONTWAIT,
                         (struct sockaddr *)&v4_peer, &v4_peerlen)) > 0) on_v4++;
    while ((n = recvfrom(v6_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL)) > 0) {
        if (n == 200) v6_transport++;
        else          v6_probe++;
    }
    fprintf(stderr, "          (after deadline: v4=%d, v6 transport=%d, v6 probe=%d)\n",
            on_v4, v6_transport, v6_probe);
    ASSERT(on_v4 >= 50 * 99 / 100);
    /* Not one byte of the tunnel may have moved to the family that never
     * answered — that is the whole point. */
    ASSERT_EQ(v6_transport, 0);

    /* The server comes back on the family the client never left. */
    uint8_t awg[200];
    make_awg_transport(awg, 0x7201, 4243, 200);
    ASSERT(sendto(v4_fd, awg, sizeof(awg), 0,
                  (struct sockaddr *)&v4_peer, v4_peerlen) > 0);

    int got_wg = 0;
    for (int i = 0; i < 10 && !got_wg; i++) {
        int r = recv_one(client_fd, buf, sizeof(buf), 1000);
        if (r != 200) continue;
        uint32_t t;
        memcpy(&t, buf, 4);
        if (t != WG_TRANSPORT_DATA) continue;
        uint64_t ctr;
        memcpy(&ctr, buf + 8, 8);
        ASSERT_EQ(ctr, 4243u);
        got_wg = 1;
    }
    ASSERT(got_wg);

    stop_proxy(proxy);
    close(client_fd);
    close(v6_fd);
    close(v4_fd);
}

/* Keep the client handshaking until the one open family sees the init, then
 * answer on it. The retries matter as much as the answer: an unanswered init is
 * exactly what tells the silence watchdog the path is gone, and the watchdog is
 * what reconnects the proxy so the probe gets to run again. Returns 1 if the
 * survivor was IPv6, 0 for IPv4, -1 if it never saw anything. */
static int handshake_on_surviving_family(int client_fd, struct sockaddr_in *proxy_addr,
                                         int live_fd, uint32_t idx, int live_is_v6,
                                         int max_attempts) {
    if (live_fd >= 0) drain_socket(live_fd);

    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, idx);
    for (int attempt = 0; attempt < max_attempts; attempt++) {
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));

        struct sockaddr_storage from;
        socklen_t fromlen = sizeof(from);
        if (!await_awg_init(live_fd, &from, &fromlen, TEST_JC + 5, 700)) continue;

        uint8_t awg[200];
        make_awg_transport(awg, idx, 1, 200);
        sendto(live_fd, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);
        usleep(400000);
        return live_is_v6;
    }
    return -1;
}

/* Push transport traffic and report where it went. */
static void measure_family_split(int client_fd, struct sockaddr_in *proxy_addr,
                                 uint32_t idx, int v4_fd, int v6_fd,
                                 int *on_v4, int *on_v6) {
    uint8_t transport[200];
    for (int i = 0; i < 30; i++) {
        make_wg_transport(transport, idx, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)proxy_addr, sizeof(*proxy_addr));
    }
    usleep(400000);

    uint8_t buf[2048];
    *on_v4 = *on_v6 = 0;
    if (v4_fd >= 0)
        while (recvfrom(v4_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) (*on_v4)++;
    if (v6_fd >= 0)
        while (recvfrom(v6_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) (*on_v6)++;
}

/* ---- Scenario 15: after a silent spell, the family that wakes up first wins ----
 *
 * The companion to the scenario above. Keeping the primary when neither family
 * answers is right, but the run must not go deaf while it waits: whichever
 * address comes back has to be picked up within seconds, not at the next
 * reconnect. Measured on the live pair, giving up outright cost the best part
 * of a minute after the path returned.
 *
 * Both families are black holes past the probe window, then IPv6 starts
 * answering. The run has to move there on its own. */
static void test_he_late_answer_wins(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    int v6_fd = make_udp_socket6(remote_port);
    if (v6_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    int v4_fd = make_udp_socket(remote_port);
    ASSERT(v4_fd >= 0);

    char rbuf[64];
    snprintf(rbuf, sizeof(rbuf), "localhost:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf, "200");
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x7300);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ASSERT(await_awg_init(v6_fd, &from, &fromlen, TEST_JC + 5, 2000));

    /* Outlast the window with both mute, so the probe has already decided to
     * keep the primary. */
    fprintf(stderr, "          (waiting out the 15s window with both silent...)\n");
    sleep(18);
    drain_socket(v4_fd);

    /* Now IPv6 wakes up. Answer the next replay that arrives. */
    int answered_at = -1;
    for (int i = 0; i < 12 && answered_at < 0; i++) {
        make_wg_init(init_buf, 0x7300);
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        fromlen = sizeof(from);
        if (await_awg_init(v6_fd, &from, &fromlen, TEST_JC + 5, 1000)) {
            uint8_t awg[200];
            make_awg_transport(awg, 0x7301, 1, 200);
            sendto(v6_fd, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);
            answered_at = i;
        }
    }
    fprintf(stderr, "          (IPv6 answered a replay on attempt %d)\n", answered_at);
    ASSERT(answered_at >= 0);          /* the probe must still have been sounding it */
    usleep(600000);
    drain_socket(v4_fd);
    drain_socket(v6_fd);

    /* Everything from here must leave over IPv6 — the family that answered. */
    uint8_t transport[200];
    for (int i = 0; i < 40; i++) {
        make_wg_transport(transport, 0x7301, (uint64_t)i, 200);
        sendto(client_fd, transport, 200, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
    }
    usleep(500000);

    int v4_transport = 0, v6_transport = 0;
    uint8_t buf[2048];
    ssize_t n;
    while ((n = recvfrom(v4_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL)) > 0)
        if (n == 200) v4_transport++;
    while ((n = recvfrom(v6_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL)) > 0)
        if (n == 200) v6_transport++;
    fprintf(stderr, "          (after the late answer: v4=%d, v6=%d)\n",
            v4_transport, v6_transport);
    ASSERT(v6_transport >= 40 * 90 / 100);
    ASSERT_EQ(v4_transport, 0);

    stop_proxy(proxy);
    close(client_fd);
    close(v4_fd);
    close(v6_fd);
}

/* ---- Scenario 16: the family only ever changes when a family answers ----
 *
 * Walks the whole matrix in one process: start on IPv4, switch to IPv6, back to
 * IPv4, and out to IPv6 again. Between rounds the family in use is taken away
 * and left down, so the client handshakes into silence and the watchdog has to
 * notice and reconnect — the same sequence a real outage produces, and the only
 * thing that lets the probe run a second time.
 *
 * The last assertion is the flash one: the router's storage is weak and the
 * learned family is documented as at most one write per run, so four verdicts
 * must still leave exactly one byte on disk. */
static void test_he_family_switch_matrix(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int probe6 = make_udp_socket6(remote_port);
    if (probe6 < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    close(probe6);

    /* One state file for the whole scenario: the point is what a single run
     * accumulates across reconnects. */
    state_file_pin();
    g_timeout = "5";   /* the outage path in seconds, not three minutes */

    int v4_fd = make_udp_socket(remote_port);
    int v6_fd = make_udp_socket6(remote_port);
    ASSERT(v4_fd >= 0 && v6_fd >= 0);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "200");
    g_timeout = "30";
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);
    int on_v4, on_v6;

    /* Round 1 — nothing learned yet, so IPv4 leads and IPv4 answers. */
    ASSERT_EQ(handshake_on_surviving_family(client_fd, &proxy_addr, v4_fd,
                                            0x8100, 0, 12), 0);
    measure_family_split(client_fd, &proxy_addr, 0x8100, v4_fd, v6_fd, &on_v4, &on_v6);
    fprintf(stderr, "          (round 1, start on IPv4: v4=%d v6=%d)\n", on_v4, on_v6);
    ASSERT(on_v4 >= 25);
    ASSERT_EQ(on_v6, 0);
    /* Staying where it started is not a change, so nothing is written. */
    ASSERT_EQ(state_file_read(), 0);

    /* Round 2 — IPv4 goes away for good; only IPv6 can answer. */
    close(v4_fd);
    v4_fd = -1;
    ASSERT_EQ(handshake_on_surviving_family(client_fd, &proxy_addr, v6_fd,
                                            0x8200, 1, 30), 1);
    measure_family_split(client_fd, &proxy_addr, 0x8200, v4_fd, v6_fd, &on_v4, &on_v6);
    fprintf(stderr, "          (round 2, switched to IPv6: v6=%d)\n", on_v6);
    ASSERT(on_v6 >= 25);
    ASSERT_EQ(state_file_read(), '6');

    /* Round 3 — IPv6 goes away, IPv4 comes back: switch back. */
    v4_fd = make_udp_socket(remote_port);
    ASSERT(v4_fd >= 0);
    close(v6_fd);
    v6_fd = -1;
    ASSERT_EQ(handshake_on_surviving_family(client_fd, &proxy_addr, v4_fd,
                                            0x8300, 0, 30), 0);
    measure_family_split(client_fd, &proxy_addr, 0x8300, v4_fd, v6_fd, &on_v4, &on_v6);
    fprintf(stderr, "          (round 3, back to IPv4: v4=%d)\n", on_v4);
    ASSERT(on_v4 >= 25);

    /* Round 4 — and out to IPv6 once more. */
    v6_fd = make_udp_socket6(remote_port);
    ASSERT(v6_fd >= 0);
    close(v4_fd);
    v4_fd = -1;
    ASSERT_EQ(handshake_on_surviving_family(client_fd, &proxy_addr, v6_fd,
                                            0x8400, 1, 30), 1);
    measure_family_split(client_fd, &proxy_addr, 0x8400, v4_fd, v6_fd, &on_v4, &on_v6);
    fprintf(stderr, "          (round 4, switched to IPv6: v6=%d)\n", on_v6);
    ASSERT(on_v6 >= 25);

    /* Four verdicts, one byte. The in-memory preference followed every one of
     * them; the flash copy was written once and never again. */
    ASSERT_EQ(state_file_read(), '6');

    stop_proxy(proxy);
    close(client_fd);
    if (v4_fd >= 0) close(v4_fd);
    if (v6_fd >= 0) close(v6_fd);
    state_file_unpin();
}

/* ---- Scenario 16: a learned IPv6 preference leads the next run's dial ----
 *
 * The flash byte exists so a site whose IPv4 is dead stops paying the probe
 * delay on every restart. Seed it, restart, and the very first packet must go
 * out over IPv6 with the IPv4 socket never touched. */
static void test_he_learned_preference_leads(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int v6_fd = make_udp_socket6(remote_port);
    if (v6_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    int v4_fd = make_udp_socket(remote_port);
    ASSERT(v4_fd >= 0);

    state_file_pin();
    int sf = open(g_state_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT(sf >= 0);
    ASSERT_EQ(write(sf, "6", 1), 1);
    close(sf);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    /* A long head start: if the dial order were still IPv4-first the IPv4
     * socket would carry the init outright, and the IPv6 replay would not be
     * due for two seconds. */
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "2000");
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x8500);
    sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
           (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));

    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    ASSERT(await_awg_init(v6_fd, &from, &fromlen, TEST_JC + 5, 1500));
    ASSERT_EQ(from.ss_family, AF_INET6);

    int on_v4 = 0;
    uint8_t buf[2048];
    while (recvfrom(v4_fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) on_v4++;
    fprintf(stderr, "          (IPv6 led the dial, v4 saw %d)\n", on_v4);
    ASSERT_EQ(on_v4, 0);

    stop_proxy(proxy);
    close(client_fd);
    close(v4_fd);
    close(v6_fd);
    state_file_unpin();
}

/* ---- Scenario 17: every (re)connect resolves the name again ----
 *
 * A remote given as a hostname may move — that is the whole reason the DNS
 * re-resolve exists — so no reconnect may reuse the address the previous one
 * happened to get. Checked against the proxy's own log: each "reconnecting to"
 * has to be followed by a fresh "resolving". */
static void test_dns_resolve_before_each_connect(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    char logpath[96];
    snprintf(logpath, sizeof(logpath), "/tmp/awg-log-%d", (int)getpid());
    unlink(logpath);

    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    g_proxy_log = logpath;
    g_log_level = "info";
    g_timeout = "5";
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "200");
    g_proxy_log = NULL;
    g_log_level = "error";
    g_timeout = "30";
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Take the server away and keep the client handshaking into the silence.
     * Unanswered inits are what the watchdog acts on, so this produces real
     * reconnects — and every one of them has to resolve the name again. */
    close(server_fd);
    server_fd = -1;
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x8600);
    for (int i = 0; i < 60; i++) {
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(400000);
    }

    stop_proxy(proxy);

    char *log = NULL;
    long len = 0;
    {
        FILE *f = fopen(logpath, "rb");
        ASSERT(f != NULL);
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        log = malloc((size_t)len + 1);
        ASSERT(log != NULL);
        ASSERT_EQ((long)fread(log, 1, (size_t)len, f), len);
        log[len] = 0;
        fclose(f);
    }

    int resolves = 0, reconnects = 0;
    for (const char *s = log; (s = strstr(s, "resolving ")) != NULL; s += 10) resolves++;
    for (const char *s = log; (s = strstr(s, "reconnecting to ")) != NULL; s += 16) reconnects++;
    fprintf(stderr, "          (reconnects=%d, resolves=%d)\n", reconnects, resolves);

    /* The reconnects have to have happened, or this proves nothing. */
    ASSERT(reconnects >= 2);
    /* One resolve for the initial dial plus one per reconnect, at least. */
    ASSERT(resolves >= reconnects + 1);

    /* And the order matters: a reconnect that reused a cached address would
     * show up as two "reconnecting to" lines with no "resolving" between. */
    {
        const char *s = log;
        for (int i = 0; i < reconnects; i++) {
            const char *rc = strstr(s, "reconnecting to ");
            ASSERT(rc != NULL);
            const char *next_rc = strstr(rc + 16, "reconnecting to ");
            const char *rs = strstr(rc + 16, "resolving ");
            ASSERT(rs != NULL);
            ASSERT(next_rc == NULL || rs < next_rc);
            s = rc + 16;
        }
    }

    free(log);
    close(client_fd);
    if (server_fd >= 0) close(server_fd);
    unlink(logpath);
}

/* ---- Scenario 18: a silent remote that sends no ICMP still gets noticed ----
 *
 * The reconnects everything else relies on come from ICMP port-unreachable,
 * which only happens when the far port is closed. A path that swallows packets
 * — a filtered route, a server whose host is up but whose process is gone, the
 * IPv4 leg of the site this was found on — produces nothing at all, and then the
 * silence watchdog is the only thing left.
 *
 * It used to be unable to fire. The two counters each zeroed the other's
 * condition, so the threshold could only be reached by a client that put a
 * packet in every single five-second tick; one quieter than that — a WireGuard
 * keepalive every 25 s, a handshake retry that drifts across a tick boundary —
 * reset the count over and over and a wedged tunnel stayed wedged for as long
 * as it was left alone.
 *
 * So the timing here is deliberate: the timeout is three ticks and the client
 * speaks once every twelve seconds, which lands in every other tick at best.
 * The old pair of counters never gets past one; the single counter counts the
 * remote's silence, which is the thing actually being measured. Both remotes
 * are bound and mute, so no ICMP can do the job instead, and the reconnect must
 * also drop the family the run had learned. */
static void test_watchdog_reconnects_without_icmp(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int v6_fd = make_udp_socket6(remote_port);
    if (v6_fd < 0) {
        fprintf(stderr, "          (no IPv6 loopback on this host, skipped)\n");
        return;
    }
    /* Bound, listening, and never answering: no ICMP will ever come back. */
    int v4_fd = make_udp_socket(remote_port);
    ASSERT(v4_fd >= 0);

    char logpath[96];
    snprintf(logpath, sizeof(logpath), "/tmp/awg-wd-%d", (int)getpid());
    unlink(logpath);

    /* Seed a learned IPv6 preference so the reset has something to undo. */
    state_file_pin();
    int sf = open(g_state_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT(sf >= 0);
    ASSERT_EQ(write(sf, "6", 1), 1);
    close(sf);

    char rbuf_env[64];
    snprintf(rbuf_env, sizeof(rbuf_env), "localhost:%d", remote_port);
    g_proxy_log = logpath;
    g_log_level = "info";
    g_timeout = "15";   /* three five-second ticks */
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf_env, "200");
    g_proxy_log = NULL;
    g_log_level = "error";
    g_timeout = "30";
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Handshake into the void, slowly. Keepalive-only traffic would be a
     * legitimately quiet tunnel; repeated inits are the tunnel saying it cannot
     * come up. Twelve seconds apart is the point — sparse enough that a counter
     * needing consecutive busy ticks can never accumulate. */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x8700);
    for (int i = 0; i < 5; i++) {
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        for (int j = 0; j < 24; j++) {
            usleep(500000);
            drain_socket(v4_fd);
            drain_socket(v6_fd);
        }
    }

    stop_proxy(proxy);

    char *log = NULL;
    {
        FILE *f = fopen(logpath, "rb");
        ASSERT(f != NULL);
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        log = malloc((size_t)len + 1);
        ASSERT(log != NULL);
        ASSERT_EQ((long)fread(log, 1, (size_t)len, f), len);
        log[len] = 0;
        fclose(f);
    }

    int reconnects = 0, resets = 0;
    for (const char *s = log; (s = strstr(s, "reconnecting to ")) != NULL; s += 16)
        reconnects++;
    for (const char *s = log; (s = strstr(s, "trying IPv4 and IPv6 afresh")) != NULL; s += 26)
        resets++;
    fprintf(stderr, "          (no ICMP anywhere: reconnects=%d, family resets=%d)\n",
            reconnects, resets);

    /* No ICMP was possible, so the watchdog is the only thing that could have
     * produced either line. */
    ASSERT(reconnects >= 1);
    ASSERT(resets >= 1);

    free(log);
    close(client_fd);
    close(v4_fd);
    close(v6_fd);
    unlink(logpath);
    state_file_unpin();
}

/* ---- Scenario 19: the remote moves — the run has to follow it ----
 *
 * A hostname remote is not a fixed address: a hub on a dynamic lease, or one
 * behind a DDNS name, moves and leaves the old address answering nobody. The
 * run re-checks its own address against the name's records and reconnects when
 * it is no longer there, which is the only thing that gets it to the new one.
 *
 * /etc/hosts is what the resolver reads first, so rewriting it moves the name
 * exactly the way a DDNS update would, with no DNS server to stand up. Both
 * addresses are loopback, so "moving" is just binding the second one. */
static void test_remote_address_moves(void) {
    /* Needs to rewrite /etc/hosts — fine as root in the test container. */
    FILE *probe = fopen("/etc/hosts", "a");
    if (!probe) {
        fprintf(stderr, "          (/etc/hosts not writable, skipped)\n");
        return;
    }
    fclose(probe);

    /* Keep the original so the box is left as it was found. */
    char *orig = NULL;
    long orig_len = 0;
    {
        FILE *f = fopen("/etc/hosts", "rb");
        ASSERT(f != NULL);
        fseek(f, 0, SEEK_END); orig_len = ftell(f); fseek(f, 0, SEEK_SET);
        orig = malloc((size_t)orig_len + 1);
        ASSERT(orig != NULL);
        ASSERT_EQ((long)fread(orig, 1, (size_t)orig_len, f), orig_len);
        orig[orig_len] = 0;
        fclose(f);
    }

    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0 && remote_port > 0);

    /* Two "hosts": 127.0.0.1 now, 127.0.0.2 after the move. */
    struct sockaddr_in a1 = make_addr(remote_port);
    struct sockaddr_in a2 = make_addr(remote_port);
    a2.sin_addr.s_addr = inet_addr("127.0.0.2");

    int fd1 = socket(AF_INET, SOCK_DGRAM, 0);
    int fd2 = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT(fd1 >= 0 && fd2 >= 0);
    int one = 1;
    setsockopt(fd1, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd2, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ASSERT_EQ(bind(fd1, (struct sockaddr *)&a1, sizeof(a1)), 0);
    ASSERT_EQ(bind(fd2, (struct sockaddr *)&a2, sizeof(a2)), 0);

    int rc = 0;
    {   /* Point the name at the first address. */
        FILE *f = fopen("/etc/hosts", "w");
        ASSERT(f != NULL);
        fprintf(f, "%s\n127.0.0.1 awghub.test\n", orig);
        fclose(f);
    }

    char rbuf[64];
    snprintf(rbuf, sizeof(rbuf), "awghub.test:%d", remote_port);
    g_dns_refresh = "5";          /* re-check every tick instead of every minute */
    pid_t proxy = start_proxy_remote("normal", listen_port, rbuf, NULL);
    g_dns_refresh = NULL;
    ASSERT(proxy > 0);

    int client_fd = make_client_socket();
    ASSERT(client_fd >= 0);
    struct sockaddr_in proxy_addr = make_addr(listen_port);

    /* Establish on the first address. */
    uint8_t init_buf[WG_INIT_SIZE];
    make_wg_init(init_buf, 0x8800);
    struct sockaddr_storage from;
    socklen_t fromlen = sizeof(from);
    int seen = 0;
    for (int i = 0; i < 8 && !seen; i++) {
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        seen = await_awg_init(fd1, &from, &fromlen, TEST_JC + 5, 700);
    }
    if (!seen) { rc = 1; goto done; }
    {
        uint8_t awg[200];
        make_awg_transport(awg, 0x8800, 1, 200);
        sendto(fd1, awg, sizeof(awg), 0, (struct sockaddr *)&from, fromlen);
    }
    usleep(300000);
    drain_socket(fd1);
    drain_socket(fd2);

    /* The hub moves. Nothing else changes — same name, same port. */
    {
        FILE *f = fopen("/etc/hosts", "w");
        ASSERT(f != NULL);
        fprintf(f, "%s\n127.0.0.2 awghub.test\n", orig);
        fclose(f);
    }
    fprintf(stderr, "          (remote moved to 127.0.0.2, waiting for the run to follow)\n");

    /* Keep handshaking at the name; the run must end up at the new address. */
    int landed = 0;
    for (int i = 0; i < 40 && !landed; i++) {
        make_wg_init(init_buf, 0x8801);
        sendto(client_fd, init_buf, WG_INIT_SIZE, 0,
               (struct sockaddr *)&proxy_addr, sizeof(proxy_addr));
        usleep(500000);
        uint8_t buf[2048];
        while (recvfrom(fd2, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL) > 0) landed++;
        drain_socket(fd1);
    }
    fprintf(stderr, "          (packets at the new address: %d)\n", landed);
    if (!landed) rc = 2;

done:
    stop_proxy(proxy);
    close(client_fd);
    close(fd1);
    close(fd2);
    {   /* Put /etc/hosts back exactly as it was. */
        FILE *f = fopen("/etc/hosts", "w");
        if (f) { fwrite(orig, 1, (size_t)orig_len, f); fclose(f); }
    }
    free(orig);
    ASSERT_EQ(rc, 0);
}

/* ---- Scenario 20: dual-stack hub — AWG_LISTEN on [::] serves both families ----
 *
 * This is the server-mode leg the MikroTik hub needs: the AWG side faces the
 * internet over IPv6 while the WireGuard side stays on the router's veth over
 * IPv4. A v4 client on the same socket arrives v4-mapped, so both must be
 * routed back by receiver_index to the family they came from. */
static void test_server_listen6(void) {
    int listen_port = find_free_port();
    int remote_port = find_free_port();
    ASSERT(listen_port > 0);
    ASSERT(remote_port > 0);

    int probe = make_client_socket6();
    if (probe < 0) {
        fprintf(stderr, "          (no IPv6 on this host, skipped)\n");
        return;
    }
    close(probe);

    /* WG side stays IPv4 — that is the veth inside the router. */
    int server_fd = make_udp_socket(remote_port);
    ASSERT(server_fd >= 0);

    char lbuf[32], rbuf[64];
    snprintf(lbuf, sizeof(lbuf), "[::]:%d", listen_port);
    snprintf(rbuf, sizeof(rbuf), "127.0.0.1:%d", remote_port);
    pid_t proxy = start_proxy_listen_remote("server", lbuf, rbuf, NULL);
    ASSERT(proxy > 0);

    int fd6 = make_client_socket6();
    int fd4 = make_client_socket();
    ASSERT(fd6 >= 0 && fd4 >= 0);
    struct sockaddr_in6 hub6 = make_addr6(listen_port);
    struct sockaddr_in  hub4 = make_addr(listen_port);

    const uint32_t idx6 = 0x6a00, idx4 = 0x4a00;
    uint8_t awg_init[TEST_S1 + WG_INIT_SIZE];

    make_awg_init(awg_init, idx6);
    ASSERT(sendto(fd6, awg_init, sizeof(awg_init), 0,
                  (struct sockaddr *)&hub6, sizeof(hub6)) > 0);
    make_awg_init(awg_init, idx4);
    ASSERT(sendto(fd4, awg_init, sizeof(awg_init), 0,
                  (struct sockaddr *)&hub4, sizeof(hub4)) > 0);
    usleep(300000);

    /* Both handshakes must have arrived at the WG server as plain WG. */
    struct sockaddr_in proxy_src;
    memset(&proxy_src, 0, sizeof(proxy_src));
    int seen6 = 0, seen4 = 0;
    for (;;) {
        uint8_t buf[2048];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        ssize_t n = recvfrom(server_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
        proxy_src = from;
        if (n != WG_INIT_SIZE) continue;   /* junk/CPS ahead of the init */
        uint32_t t, si;
        memcpy(&t, buf, 4);
        memcpy(&si, buf + 4, 4);
        if (t != WG_HANDSHAKE_INIT) continue;
        if (si == idx6) seen6 = 1;
        if (si == idx4) seen4 = 1;
    }
    fprintf(stderr, "          (WG init seen: v6-client=%d, v4-client=%d)\n", seen6, seen4);
    ASSERT(seen6);
    ASSERT(seen4);
    ASSERT(proxy_src.sin_port != 0);

    /* Reply to each by receiver_index; each must come back on its own family. */
    uint8_t wg[200];
    for (int i = 0; i < 50; i++) {
        make_wg_transport(wg, idx6, (uint64_t)i, 200);
        sendto(server_fd, wg, 200, 0, (struct sockaddr *)&proxy_src, sizeof(proxy_src));
        make_wg_transport(wg, idx4, (uint64_t)i, 200);
        sendto(server_fd, wg, 200, 0, (struct sockaddr *)&proxy_src, sizeof(proxy_src));
    }
    usleep(400000);

    /* Count only packets carrying the receiver_index this client owns: a
     * session-table mix-up would show up as a delivery to the wrong family. */
    int got6 = 0, got4 = 0, misrouted = 0;
    struct { int fd; uint32_t mine; uint32_t other; int *hit; } side[2] = {
        { fd6, idx6, idx4, &got6 }, { fd4, idx4, idx6, &got4 }
    };
    for (int s = 0; s < 2; s++) {
        uint8_t buf[2048];
        ssize_t n;
        while ((n = recvfrom(side[s].fd, buf, sizeof(buf), MSG_DONTWAIT, NULL, NULL)) > 0) {
            if (n != 200) continue;         /* junk ahead of the stream */
            uint32_t ri;
            memcpy(&ri, buf + 4, 4);
            if (ri == side[s].mine) (*side[s].hit)++;
            else if (ri == side[s].other) misrouted++;
        }
    }
    fprintf(stderr, "          (routed back: v6=%d, v4=%d of 50 each, misrouted=%d)\n",
            got6, got4, misrouted);
    ASSERT(got6 >= 50 * 9 / 10);
    ASSERT(got4 >= 50 * 9 / 10);
    ASSERT_EQ(misrouted, 0);

    stop_proxy(proxy);
    close(fd6);
    close(fd4);
    close(server_fd);
}

int main(void) {
    fprintf(stderr, "=== stress tests ===\n");
    RUN_TEST(normal_burst);
    RUN_TEST(reverse_bidirectional);
    RUN_TEST(server_multiclient);
    RUN_TEST(server_rekey);
    RUN_TEST(concurrent_handshakes);
    RUN_TEST(server_handshake_batch);
    RUN_TEST(scale);
    RUN_TEST(gso_connected);
    RUN_TEST(gro_bidirectional);
    RUN_TEST(throughput_benchmark);
    RUN_TEST(s2s_fallback);
    RUN_TEST(v3_header_protection);
    RUN_TEST(v31_random_trailers);
    RUN_TEST(ipv6_remote);
    RUN_TEST(happy_eyeballs);
    RUN_TEST(he_both_silent_keeps_primary);
    RUN_TEST(he_late_answer_wins);
    RUN_TEST(he_family_switch_matrix);
    RUN_TEST(he_learned_preference_leads);
    RUN_TEST(dns_resolve_before_each_connect);
    RUN_TEST(watchdog_reconnects_without_icmp);
    RUN_TEST(remote_address_moves);
    RUN_TEST(server_listen6);
    TEST_MAIN_END();
}
