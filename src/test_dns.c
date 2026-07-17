#include <stdint.h>
#include <arpa/inet.h>
#include "test.h"
#include "proxy.h"

static int check(const char *host, const char *cur_ip) {
    struct in_addr cur;
    inet_pton(AF_INET, cur_ip, &cur);
    return resolve_addr_check(host, &cur);
}

static void test_literal_match(void) {
    /* Literal IP resolves to itself — still present */
    ASSERT_EQ(check("127.0.0.1", "127.0.0.1"), 0);
}

static void test_literal_mismatch(void) {
    /* Current IP is not among the records — gone */
    ASSERT_EQ(check("127.0.0.1", "10.9.9.9"), 1);
}

static void test_hosts_file(void) {
    /* localhost comes from /etc/hosts, no network needed */
    ASSERT_EQ(check("localhost", "127.0.0.1"), 0);
}

static void test_resolve_error(void) {
    /* Empty name fails without touching the network */
    ASSERT_EQ(check("", "127.0.0.1"), -1);
}

int main(void) {
    fprintf(stderr, "=== dns tests ===\n");
    RUN_TEST(literal_match);
    RUN_TEST(literal_mismatch);
    RUN_TEST(hosts_file);
    RUN_TEST(resolve_error);
    TEST_MAIN_END();
}
