/* Learned transport preference persisted across restarts. The file is the only
 * thing that survives a container restart, so its exact contract matters:
 * anything that is not a literal '6' must read as "no preference" and leave the
 * stock IPv4-first order alone. */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "test.h"
#include "proxy.h"

#define PATH "/tmp/awg_test_state"

static void put(const char *s) {
    int fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    (void)!write(fd, s, strlen(s));
    close(fd);
}

static void test_missing_file_means_no_preference(void) {
    unlink(PATH);
    ASSERT_EQ(state_read_prefer6(PATH), 0);
}

static void test_empty_path_is_ignored(void) {
    ASSERT_EQ(state_read_prefer6(""), 0);
    ASSERT_EQ(state_read_prefer6(NULL), 0);
    /* Writing nowhere is an error, not a silent success. */
    ASSERT_EQ(state_write("", 1, NULL), -1);
    ASSERT_EQ(state_write(NULL, 1, NULL), -1);
}

static void test_ipv6_round_trip(void) {
    unlink(PATH);
    ASSERT_EQ(state_write(PATH, 1, NULL), 0);
    ASSERT_EQ(state_read_prefer6(PATH), 1);
}

/* Unlearning has to work as well as learning: an IPv6 route that stops
 * carrying traffic must not keep winning the dial order forever. Note this
 * restores the stock order rather than disabling IPv6 — the probe still runs. */
static void test_ipv4_overwrites_ipv6(void) {
    ASSERT_EQ(state_write(PATH, 1, NULL), 0);
    ASSERT_EQ(state_write(PATH, 0, NULL), 0);
    ASSERT_EQ(state_read_prefer6(PATH), 0);
}

static void test_one_byte_only(void) {
    ASSERT_EQ(state_write(PATH, 1, NULL), 0);
    int fd = open(PATH, O_RDONLY);
    ASSERT(fd >= 0);
    char buf[8];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    ASSERT_EQ((int)n, 1);
    ASSERT_EQ(buf[0], '6');
}

/* A truncated or garbled file must not be read as "prefer IPv6" — the safe
 * direction is always back to the stock order. */
static void test_garbage_reads_as_no_preference(void) {
    put("");
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    put("x");
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    put("46");
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    put("6");
    ASSERT_EQ(state_read_prefer6(PATH), 1);
}

static void test_unwritable_path_reports_failure(void) {
    ASSERT_EQ(state_write("/proc/nonexistent/awg.state", 1, NULL), -1);
}

/* Cloudflare WARP's endpoint rides on a second line. It must not disturb the
 * preference byte, and the preference byte must not drop it. */
static void test_warp_endpoint_round_trip(void) {
    char ep[64];
    ASSERT_EQ(state_write(PATH, 1, "2408 162.159.192.50"), 0);
    ASSERT_EQ(state_read_prefer6(PATH), 1);
    state_read_ep(PATH, ep, sizeof(ep));
    ASSERT(strcmp(ep, "2408 162.159.192.50") == 0);
    ASSERT_EQ(state_write(PATH, 0, "500 2606:4700:d0::a29f:c001"), 0);
    ASSERT_EQ(state_read_prefer6(PATH), 0);
    state_read_ep(PATH, ep, sizeof(ep));
    ASSERT(strcmp(ep, "500 2606:4700:d0::a29f:c001") == 0);
}

/* A file from an older release, or a bare preference, has no endpoint. */
static void test_no_endpoint_line(void) {
    char ep[64];
    put("6");
    state_read_ep(PATH, ep, sizeof(ep));
    ASSERT_EQ(ep[0], 0);
    put("6x2408 1.2.3.4");
    state_read_ep(PATH, ep, sizeof(ep));
    ASSERT_EQ(ep[0], 0);
    unlink(PATH);
    state_read_ep(PATH, ep, sizeof(ep));
    ASSERT_EQ(ep[0], 0);
    state_read_ep("", ep, sizeof(ep));
    ASSERT_EQ(ep[0], 0);
}

/* The reader stops at the buffer it was given, whatever the file holds. */
static void test_endpoint_truncated_to_buffer(void) {
    char ep[8];
    ASSERT_EQ(state_write(PATH, 0, "2408 162.159.192.50"), 0);
    state_read_ep(PATH, ep, sizeof(ep));
    ASSERT(strcmp(ep, "2408 16") == 0);
}

int main(void) {
    fprintf(stderr, "=== state file tests ===\n");
    RUN_TEST(missing_file_means_no_preference);
    RUN_TEST(empty_path_is_ignored);
    RUN_TEST(ipv6_round_trip);
    RUN_TEST(ipv4_overwrites_ipv6);
    RUN_TEST(one_byte_only);
    RUN_TEST(garbage_reads_as_no_preference);
    RUN_TEST(unwritable_path_reports_failure);
    RUN_TEST(warp_endpoint_round_trip);
    RUN_TEST(no_endpoint_line);
    RUN_TEST(endpoint_truncated_to_buffer);
    unlink(PATH);
    TEST_MAIN_END();
}
