#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "test.h"
#include "transform.h"
#include "blake2s.h"
#include "proxy.h"

/* Session table + reverse-mode MAC1 tests.
 *
 * The first half models the table with a portable struct (probing, eviction,
 * collisions); the second half exercises the real one from proxy.h, which now
 * stores a cliaddr_t because the client leg can be IPv6. */

/* --- Table behaviour, modelled --- */

typedef struct {
    uint32_t sender_index;
    uint32_t ip;
    uint16_t port;
    int valid;
} test_session_t;

static test_session_t g_sessions[SESSION_TABLE_SIZE];

static void mock_session_put(uint32_t index, uint32_t ip, uint16_t port) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (!g_sessions[s].valid || g_sessions[s].sender_index == index) {
            g_sessions[s].sender_index = index;
            g_sessions[s].ip = ip;
            g_sessions[s].port = port;
            g_sessions[s].valid = 1;
            return;
        }
    }
    g_sessions[slot].sender_index = index;
    g_sessions[slot].ip = ip;
    g_sessions[slot].port = port;
    g_sessions[slot].valid = 1;
}

static test_session_t *mock_session_get(uint32_t index) {
    uint32_t slot = index & SESSION_TABLE_MASK;
    for (int i = 0; i < 4; i++) {
        uint32_t s = (slot + i) & SESSION_TABLE_MASK;
        if (g_sessions[s].valid && g_sessions[s].sender_index == index)
            return &g_sessions[s];
    }
    return NULL;
}

static void reset_sessions(void) {
    memset(g_sessions, 0, sizeof(g_sessions));
}

/* 1. Basic put/get */
static void test_session_basic(void) {
    reset_sessions();
    mock_session_put(42, 0x0A000001, 12345);
    test_session_t *got = mock_session_get(42);
    ASSERT(got != NULL);
    ASSERT_EQ(got->ip, 0x0A000001u);
    ASSERT_EQ(got->port, 12345);
}

/* 2. Get non-existent */
static void test_session_miss(void) {
    reset_sessions();
    ASSERT(mock_session_get(999) == NULL);
}

/* 3. Update existing */
static void test_session_update(void) {
    reset_sessions();
    mock_session_put(100, 0x0A000001, 12345);
    mock_session_put(100, 0x0A000002, 54321);
    test_session_t *got = mock_session_get(100);
    ASSERT(got != NULL);
    ASSERT_EQ(got->ip, 0x0A000002u);
    ASSERT_EQ(got->port, 54321);
}

/* 4. Multiple entries */
static void test_session_multiple(void) {
    reset_sessions();
    for (int i = 0; i < 10; i++)
        mock_session_put((uint32_t)(i * 1000 + 7), 0x0A000000 + i, 1000 + i);

    for (int i = 0; i < 10; i++) {
        test_session_t *got = mock_session_get((uint32_t)(i * 1000 + 7));
        ASSERT(got != NULL);
        ASSERT_EQ(got->ip, (uint32_t)(0x0A000000 + i));
        ASSERT_EQ(got->port, 1000 + i);
    }
}

/* 5. Collision handling (same lower bits) */
static void test_session_collision(void) {
    reset_sessions();
    uint32_t idx1 = 5;
    uint32_t idx2 = 5 + SESSION_TABLE_SIZE;

    mock_session_put(idx1, 0x0A000001, 1111);
    mock_session_put(idx2, 0x0A000002, 2222);

    test_session_t *got1 = mock_session_get(idx1);
    test_session_t *got2 = mock_session_get(idx2);
    ASSERT(got1 != NULL);
    ASSERT(got2 != NULL);
    ASSERT_EQ(got1->port, 1111);
    ASSERT_EQ(got2->port, 2222);
}

/* 6. Eviction on full probe */
static void test_session_eviction(void) {
    reset_sessions();
    /* Fill 4 consecutive slots with same hash bucket */
    for (int i = 0; i < 4; i++)
        mock_session_put((uint32_t)(5 + i * SESSION_TABLE_SIZE), 0x01010100 + i, 100 + i);

    /* 5th entry should evict slot 5 */
    mock_session_put((uint32_t)(5 + 4 * SESSION_TABLE_SIZE), 0x02020200, 9999);
    test_session_t *got = mock_session_get((uint32_t)(5 + 4 * SESSION_TABLE_SIZE));
    ASSERT(got != NULL);
    ASSERT_EQ(got->port, 9999);
}

/* 7. transform_inbound recomputes MAC1 for init in reverse mode */
static void test_reverse_inbound_init_mac1(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s1 = 20; cfg.s2 = 20;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    memset(cfg.server_pub, 0xAA, 32);
    memset(cfg.client_pub, 0xBB, 32);
    cfg.mode = AWG_MODE_REVERSE;
    config_compute(&cfg);

    uint8_t buf[20 + WG_INIT_SIZE];
    memset(buf, 0x55, 20);
    uint32_t h1 = cfg.h1.min;
    memcpy(buf + 20, &h1, 4);
    for (int i = 4; i < WG_INIT_SIZE; i++)
        buf[20 + i] = (uint8_t)i;

    int out_len;
    uint8_t *out = transform_inbound(buf, 20 + WG_INIT_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);

    uint32_t msg_type;
    memcpy(&msg_type, out, 4);
    ASSERT_EQ(msg_type, WG_HANDSHAKE_INIT);

    /* Verify MAC1 was recomputed with server key (responder = WG server) */
    uint8_t expected_mac1[16];
    blake2s_128mac(cfg.mac1key_server, out, 116, expected_mac1);
    ASSERT_MEM_EQ(out + 116, expected_mac1, 16);
}

/* 8. Normal mode DOES recompute MAC1 for inbound init (client key) */
static void test_normal_inbound_init_mac1(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s1 = 20; cfg.s2 = 20;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    memset(cfg.client_pub, 0xBB, 32);
    cfg.mode = AWG_MODE_NORMAL;
    config_compute(&cfg);

    uint8_t buf[20 + WG_INIT_SIZE];
    memset(buf, 0x55, 20);
    uint32_t h1 = cfg.h1.min;
    memcpy(buf + 20, &h1, 4);
    for (int i = 4; i < WG_INIT_SIZE; i++)
        buf[20 + i] = (uint8_t)i;

    int out_len;
    uint8_t *out = transform_inbound(buf, 20 + WG_INIT_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);

    /* Verify MAC1 was recomputed with client key (recipient = WG client) */
    uint8_t expected_mac1[16];
    blake2s_128mac(cfg.mac1key_client, out, 116, expected_mac1);
    ASSERT_MEM_EQ(out + 116, expected_mac1, 16);
}

/* 9. Server mode also recomputes MAC1 for inbound init */
static void test_server_inbound_init_mac1(void) {
    awg_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.s1 = 20; cfg.s2 = 20;
    cfg.h1 = (hrange_t){1234567890, 1234567890};
    cfg.h2 = (hrange_t){1234567891, 1234567891};
    cfg.h3 = (hrange_t){1234567892, 1234567892};
    cfg.h4 = (hrange_t){1234567893, 1234567893};
    memset(cfg.server_pub, 0xAA, 32);
    memset(cfg.client_pub, 0xBB, 32);
    cfg.mode = AWG_MODE_SERVER;
    config_compute(&cfg);

    uint8_t buf[20 + WG_INIT_SIZE];
    memset(buf, 0x55, 20);
    uint32_t h1 = cfg.h1.min;
    memcpy(buf + 20, &h1, 4);
    for (int i = 4; i < WG_INIT_SIZE; i++)
        buf[20 + i] = (uint8_t)i;

    int out_len;
    uint8_t *out = transform_inbound(buf, 20 + WG_INIT_SIZE, &cfg, &out_len);
    ASSERT(out != NULL);
    ASSERT_EQ(out_len, WG_INIT_SIZE);

    uint8_t expected_mac1[16];
    blake2s_128mac(cfg.mac1key_server, out, 116, expected_mac1);
    ASSERT_MEM_EQ(out + 116, expected_mac1, 16);
}

/* --- The real table, the one the proxy runs on ---
 *
 * Everything above models the table with a portable struct. Since the client
 * leg can be IPv6 the entries hold a cliaddr_t, and that part is worth testing
 * against the actual code rather than a stand-in. */

static proxy_t g_p;

static cliaddr_t addr_v4(uint32_t ip, uint16_t port) {
    cliaddr_t a;
    memset(&a, 0, sizeof(a));
    a.v4.sin_family = AF_INET;
    a.v4.sin_addr.s_addr = htonl(ip);
    a.v4.sin_port = htons(port);
    return a;
}

static cliaddr_t addr_v6(const char *s, uint16_t port) {
    cliaddr_t a;
    memset(&a, 0, sizeof(a));
    a.v6.sin6_family = AF_INET6;
    a.v6.sin6_port = htons(port);
    inet_pton(AF_INET6, s, &a.v6.sin6_addr);
    return a;
}

/* 10. A v6 client survives the round trip through the real table */
/* A server-initiated handshake is aimed by peer, not by receiver_index. */
static void test_peer_lookup(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t a = addr_v6("2001:db8::1", 1111);
    cliaddr_t b = addr_v6("2001:db8::2", 2222);
    session_entry_t *ea = session_put_prof(&g_p, 0xAAAA, &a, 0);
    session_entry_t *eb = session_put_prof(&g_p, 0xBBBB, &b, 0);
    session_set_peer_slot(ea, 0);
    session_set_peer_slot(eb, 1);

    ASSERT(session_find_by_peer(&g_p, 0) == ea);
    ASSERT(session_find_by_peer(&g_p, 1) == eb);
    ASSERT(session_find_by_peer(&g_p, 2) == NULL);
    ASSERT(session_find_by_peer(&g_p, -1) == NULL);
    /* Two clients: the old sole-client fallback has nothing to offer here. */
    ASSERT(session_find_sole_entry(&g_p) == NULL);
}

/* The address churns on this link, and nothing else ever expires an entry:
 * without retiring the old one the table permanently looks like two clients. */
static void test_moved_peer_is_retired(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t old = addr_v6("2a00:1370:8180:18b::1", 41822);
    cliaddr_t neu = addr_v6("2a00:1370:8180:18b::2", 41822);
    session_entry_t *e1 = session_put_prof(&g_p, 0x1111, &old, 0);
    session_set_peer_slot(e1, 0);
    session_entry_t *e2 = session_put_prof(&g_p, 0x2222, &neu, 0);
    session_set_peer_slot(e2, 0);
    ASSERT(session_find_sole_entry(&g_p) == NULL);   /* выглядит как два клиента */

    session_drop_moved_peer(&g_p, 0, &neu);

    ASSERT(session_get(&g_p, 0x1111) == NULL);       /* старый адрес снят */
    ASSERT(session_get(&g_p, 0x2222) != NULL);
    ASSERT(session_find_sole_entry(&g_p) == e2);     /* снова один клиент */
}

/* A plain rekey keeps the previous session alive for packets still in flight,
 * so entries at the address the client still uses must survive. */
static void test_rekey_at_same_address_keeps_both(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t c = addr_v6("2001:db8::9", 51820);
    session_entry_t *e1 = session_put_prof(&g_p, 0x3333, &c, 0);
    session_set_peer_slot(e1, 0);
    session_entry_t *e2 = session_put_prof(&g_p, 0x4444, &c, 0);
    session_set_peer_slot(e2, 0);

    session_drop_moved_peer(&g_p, 0, &c);

    ASSERT(session_get(&g_p, 0x3333) != NULL);
    ASSERT(session_get(&g_p, 0x4444) != NULL);
}

/* Server mode: a client's transport names its session by OUR index, so that
 * index has to find the entry — but only while the slot still holds that
 * session. */
static void test_server_index_lookup(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t a = addr_v4(0x0A000001, 40000);
    cliaddr_t b = addr_v4(0x0A000009, 40009);
    session_entry_t *e = session_put_prof(&g_p, 0x1000, &a, 0);
    session_set_server_index(&g_p, e, 0xBEEF);

    ASSERT(session_get_by_server_index(&g_p, 0xBEEF) == e);
    ASSERT(session_get_by_server_index(&g_p, 0xBEEE) == NULL);
    ASSERT(session_get_by_server_index(&g_p, 0) == NULL);   /* 0 = неизвестен */

    /* Слот занял другой сеанс: старый индекс не должен его находить. */
    atomic_store(&e->valid, 0);
    session_entry_t *e2 = session_put_prof(&g_p, 0x1000 + SESSION_TABLE_SIZE, &b, 0);
    ASSERT(e2 == e);
    ASSERT(session_get_by_server_index(&g_p, 0xBEEF) == NULL);
}

/* The client's proxy restarted onto a new port: its transport arrives from the
 * new address under the same session, and replies must follow — the previous
 * session of the same client too, and nobody else. */
static void test_follow_moved_client(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t old = addr_v4(0x0A000002, 40001);
    cliaddr_t neu = addr_v4(0x0A000002, 40999);
    cliaddr_t other = addr_v4(0x0A000003, 40001);
    session_entry_t *prev = session_put_prof(&g_p, 0x2001, &old, 0);  /* до смены ключей */
    session_entry_t *cur = session_put_prof(&g_p, 0x2002, &old, 0);
    session_entry_t *o = session_put_prof(&g_p, 0x3001, &other, 0);
    session_set_server_index(&g_p, cur, 0xCAFE);

    ASSERT(session_follow_client(&g_p, 0xCAFE, &neu) == 1);
    ASSERT(cliaddr_eq(&cur->addr, &neu));
    ASSERT(cliaddr_eq(&prev->addr, &neu));      /* тот же клиент — переехал целиком */
    ASSERT(cliaddr_eq(&o->addr, &other));       /* чужой клиент не тронут */
    ASSERT(session_follow_client(&g_p, 0xCAFE, &neu) == 0);  /* уже там */
    ASSERT(session_get(&g_p, 0x2002) != NULL);  /* ответы по индексу клиента идут */
    ASSERT(cliaddr_eq(session_get(&g_p, 0x2002), &neu));
}

/* Transport under an index nobody recorded moves nothing: junk, or a session
 * from before a restart, must not drag a client away. */
static void test_follow_unknown_index_is_ignored(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t a = addr_v4(0x0A000004, 40002);
    cliaddr_t b = addr_v4(0x0A000005, 40003);
    session_entry_t *e = session_put_prof(&g_p, 0x4001, &a, 0);
    session_set_server_index(&g_p, e, 0xD00D);

    ASSERT(session_follow_client(&g_p, 0xD00E, &b) == 0);
    ASSERT(session_follow_client(&g_p, 0, &b) == 0);
    ASSERT(cliaddr_eq(&e->addr, &a));
}

/* A single-client hub aims server-initiated handshakes at "the only client".
 * After a move that client must still look like one client, not two. */
static void test_follow_keeps_sole_client(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t old = addr_v6("2001:db8::7", 50000);
    cliaddr_t neu = addr_v6("2001:db8::7", 50001);
    session_put_prof(&g_p, 0x5001, &old, 0);
    session_entry_t *cur = session_put_prof(&g_p, 0x5002, &old, 0);
    session_set_server_index(&g_p, cur, 0xF00D);

    ASSERT(session_follow_client(&g_p, 0xF00D, &neu) == 1);
    session_entry_t *sole = session_find_sole_entry(&g_p);
    ASSERT(sole != NULL);
    ASSERT(cliaddr_eq(&sole->addr, &neu));
}

static void test_real_session_v6(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t c6 = addr_v6("2a00:f2a:e08e:3da0::2", 51820);
    session_put(&g_p, 0x1111, &c6);

    cliaddr_t *got = session_get(&g_p, 0x1111);
    ASSERT(got != NULL);
    ASSERT_EQ(got->sa.sa_family, AF_INET6);
    ASSERT(cliaddr_eq(got, &c6));
    ASSERT_EQ(cliaddr_len(got), sizeof(struct sockaddr_in6));
    ASSERT_EQ(ntohs(cliaddr_port(got)), 51820);
}

/* 11. Both families coexist and never collapse into each other */
static void test_real_session_mixed(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t c6 = addr_v6("2001:db8::1", 1234);
    cliaddr_t c4 = addr_v4(0x0A000001, 1234);
    session_put(&g_p, 0xAAA, &c6);
    session_put(&g_p, 0xBBB, &c4);

    ASSERT(cliaddr_eq(session_get(&g_p, 0xAAA), &c6));
    ASSERT(cliaddr_eq(session_get(&g_p, 0xBBB), &c4));
    ASSERT(!cliaddr_eq(&c6, &c4));
    /* Two clients, so there is no sole one to fall back to */
    ASSERT(session_find_sole_client(&g_p) == NULL);
}

/* 12. Same v6 client under several indices still counts as one */
static void test_real_session_sole_v6(void) {
    memset(&g_p, 0, sizeof(g_p));
    cliaddr_t c6 = addr_v6("fd11:11:11::5", 500);
    session_put(&g_p, 1, &c6);
    session_put(&g_p, 2, &c6);
    cliaddr_t *sole = session_find_sole_client(&g_p);
    ASSERT(sole != NULL);
    ASSERT(cliaddr_eq(sole, &c6));

    /* One byte of the address apart is a different client */
    cliaddr_t other = addr_v6("fd11:11:11::6", 500);
    session_put(&g_p, 3, &other);
    ASSERT(session_find_sole_client(&g_p) == NULL);
}

/* 13. A port or an address bit must move the profile-cache key */
static void test_prof_cache_key_v6(void) {
    cliaddr_t a = addr_v6("2001:db8::1", 1000);
    cliaddr_t b = addr_v6("2001:db8::2", 1000);
    cliaddr_t c = addr_v6("2001:db8::1", 1001);
    ASSERT_EQ(prof_cache_key(&a), prof_cache_key(&a));
    ASSERT(prof_cache_key(&a) != prof_cache_key(&b));
    ASSERT(prof_cache_key(&a) != prof_cache_key(&c));
    /* Never 0 — that value marks an empty slot */
    cliaddr_t zero;
    memset(&zero, 0, sizeof(zero));
    zero.v6.sin6_family = AF_INET6;
    ASSERT(prof_cache_key(&zero) != 0);
}

int main(void) {
    fprintf(stderr, "=== session & reverse tests ===\n");
    RUN_TEST(session_basic);
    RUN_TEST(session_miss);
    RUN_TEST(session_update);
    RUN_TEST(session_multiple);
    RUN_TEST(session_collision);
    RUN_TEST(session_eviction);
    RUN_TEST(reverse_inbound_init_mac1);
    RUN_TEST(normal_inbound_init_mac1);
    RUN_TEST(server_inbound_init_mac1);
    RUN_TEST(peer_lookup);
    RUN_TEST(moved_peer_is_retired);
    RUN_TEST(rekey_at_same_address_keeps_both);
    RUN_TEST(server_index_lookup);
    RUN_TEST(follow_moved_client);
    RUN_TEST(follow_unknown_index_is_ignored);
    RUN_TEST(follow_keeps_sole_client);
    RUN_TEST(real_session_v6);
    RUN_TEST(real_session_mixed);
    RUN_TEST(real_session_sole_v6);
    RUN_TEST(prof_cache_key_v6);
    TEST_MAIN_END();
}
