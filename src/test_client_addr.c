/* Exercise the real reply paths while a client changes its UDP endpoint.
 * Test wrappers force the move between packets without changing proxy.c
 * or relying on a scheduler or sleep to hit the window. */
#include "proxy.h"
static ssize_t roam_sendto(int fd, const void *buf, size_t len, int flags,
                           const struct sockaddr *addr, socklen_t addrlen);
static uint8_t *roam_transform_inbound(uint8_t *pkt, int n, const awg_config_t *cfg,
                                       const awg_profile_t *pr, awg_hp_ks_t *ks,
                                       int *out_len);
#define sendto roam_sendto
#define transform_inbound_profile roam_transform_inbound
#include "proxy.c"
#undef transform_inbound_profile
#undef sendto
#include "test.h"
#undef NDEBUG
#include <assert.h>

static proxy_t *roam_proxy;
static cliaddr_t roam_next;
static enum { ROAM_IDLE, ROAM_REVERSE, ROAM_NORMAL } roam_point;

static void *roam_client(void *arg) {
    assert(note_client_addr(arg, &roam_next));
    return NULL;
}

static void roam_now(void) {
    pthread_t client;
    roam_point = ROAM_IDLE;
    assert(pthread_create(&client, NULL, roam_client, roam_proxy) == 0);
    assert(pthread_join(client, NULL) == 0);
}

static ssize_t roam_sendto(int fd, const void *buf, size_t len, int flags,
                           const struct sockaddr *addr, socklen_t addrlen) {
    ssize_t sent = sendto(fd, buf, len, flags, addr, addrlen);
    if (roam_point == ROAM_REVERSE && fd == roam_proxy->listen_fd) {
        /* The cover packet already went to the old address. Its handshake
         * must go there too, even though we learn the new address now. */
        roam_now();
    }
    return sent;
}

static uint8_t *roam_transform_inbound(uint8_t *pkt, int n, const awg_config_t *cfg,
                                       const awg_profile_t *pr, awg_hp_ks_t *ks,
                                       int *out_len) {
    if (roam_point == ROAM_NORMAL && cfg == roam_proxy->cfg) {
        /* The transport packet used the fast path. Move while processing the
         * handshake behind it, before that packet chooses its destination. */
        roam_now();
    }
    return transform_inbound_profile(pkt, n, cfg, pr, ks, out_len);
}

static int roam_udp_socket(cliaddr_t *addr) {
    *addr = (cliaddr_t){ .v4 = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) } };
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    socklen_t len = sizeof(addr->v4);
    assert(bind(fd, &addr->sa, len) == 0);
    assert(getsockname(fd, &addr->sa, &len) == 0);
    return fd;
}

static void roam_setup(proxy_t *p, awg_config_t *cfg, int clients[2]) {
    roam_proxy = p;
    cliaddr_t old, unused;
    config_compute(cfg);
    assert(proxy_init(roam_proxy, cfg, "127.0.0.1:1", "127.0.0.1:2,3", -1) == 0);
    roam_proxy->listen_fd = roam_udp_socket(&unused);
    clients[0] = roam_udp_socket(&old);
    clients[1] = roam_udp_socket(&roam_next);
    assert(note_client_addr(roam_proxy, &old));
}

static void roam_cleanup(const int clients[2]) {
    if (roam_proxy->he_evfd >= 0) close(roam_proxy->he_evfd);
    close(roam_proxy->listen_fd);
    close(clients[0]);
    close(clients[1]);
    pthread_mutex_destroy(&roam_proxy->remote_mu);
    roam_proxy = NULL;
}

static void test_reverse_cover_and_handshake_share_destination(void) {
    cps_template_t cover;
    ASSERT_EQ(cps_parse("<b 0x0844>", &cover), 0);
    awg_config_t cfg = {
        .mode = AWG_MODE_REVERSE, .cps = { &cover },
        .h1 = { 11, 11 }, .h2 = { 12, 12 }, .h3 = { 13, 13 }, .h4 = { 14, 14 },
    };
    int clients[2];
    static proxy_t p;
    roam_setup(&p, &cfg, clients);

    uint8_t init[WG_INIT_SIZE] = { WG_HANDSHAKE_INIT }, rx[WG_INIT_SIZE];
    int nsend = 0;
    roam_point = ROAM_REVERSE;
    int processed = process_s2c_pkt_reverse(roam_proxy, init, init, sizeof(init), 0,
                                            roam_proxy->send_s2c.iovecs,
                                            roam_proxy->send_s2c.addrs, &nsend);
    ssize_t cover_len = recv(clients[0], rx, sizeof(rx), MSG_DONTWAIT);
    int cover_ok = cover_len == 2 && rx[0] == 0x08 && rx[1] == 0x44;
    ssize_t init_len = recv(clients[0], rx, sizeof(rx), MSG_DONTWAIT);
    int init_ok = init_len == WG_INIT_SIZE && rx[0] == 11;
    ssize_t new_len = recv(clients[1], rx, sizeof(rx), MSG_DONTWAIT);
    roam_cleanup(clients);

    ASSERT_EQ(processed, 1);
    ASSERT_EQ(nsend, 0);
    ASSERT(cover_ok);
    ASSERT(init_ok);
    ASSERT_EQ(new_len, -1);
}

static void test_normal_rotation_batch_shares_destination(void) {
    awg_config_t cfg = {
        .mode = AWG_MODE_NORMAL, .no_gso = 1,
        .rot_t_lo = 5, .rot_t_hi = 5,
        .h1 = { 11, 11 }, .h2 = { 12, 12 }, .h3 = { 13, 13 }, .h4 = { 14, 14 },
    };
    int clients[2], remote[2];
    static proxy_t p;
    roam_setup(&p, &cfg, clients);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, remote), 0);

    uint8_t data[WG_TRANSPORT_MIN] = { 14 }, resp[WG_RESP_SIZE] = { 12 }, rx[WG_RESP_SIZE];
    ASSERT_EQ(send(remote[1], data, sizeof(data), 0), (ssize_t)sizeof(data));
    ASSERT_EQ(send(remote[1], resp, sizeof(resp), 0), (ssize_t)sizeof(resp));
    roam_point = ROAM_NORMAL;
    rot_drain_old(&p, remote[0]);
    ssize_t data_len = recv(clients[0], rx, sizeof(rx), MSG_DONTWAIT);
    int data_ok = data_len == WG_TRANSPORT_MIN && rx[0] == WG_TRANSPORT_DATA;
    ssize_t resp_len = recv(clients[0], rx, sizeof(rx), MSG_DONTWAIT);
    int resp_ok = resp_len == WG_RESP_SIZE && rx[0] == WG_HANDSHAKE_RESPONSE;
    ssize_t new_len = recv(clients[1], rx, sizeof(rx), MSG_DONTWAIT);
    close(remote[0]);
    close(remote[1]);
    roam_cleanup(clients);

    ASSERT(data_ok);
    ASSERT(resp_ok);
    ASSERT_EQ(new_len, -1);
}

int main(void) {
    g_log_level = LOG_NONE;
    RUN_TEST(reverse_cover_and_handshake_share_destination);
    RUN_TEST(normal_rotation_batch_shares_destination);
    TEST_MAIN_END();
}
