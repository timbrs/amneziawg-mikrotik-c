/* Which packets of a batch one UDP_SEGMENT send may carry.
 *
 * The rules are the kernel's, not ours, and getting one wrong is expensive in
 * a way that is hard to see: a run that is too long earns EINVAL/EMSGSIZE for
 * the whole batch, and a run that ends too early silently costs the offload.
 * Neither shows up in a throughput number without a lot of staring, so pin
 * them here.
 *
 * The counting rule itself: gso_run_len returns how many packets go in one
 * send, so "the run stops before packet 3" means it returns 3. */
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "test.h"
#include "proxy.h"

#define MAXP 200
static uint8_t scratch[MAXP][2048];
static struct iovec iov[MAXP];
static cliaddr_t addrs[MAXP];

/* count packets of `len` bytes, all to the same address */
static void fill(int count, size_t len) {
    for (int i = 0; i < count; i++) {
        iov[i].iov_base = scratch[i % MAXP];
        iov[i].iov_len = len;
        memset(&addrs[i], 0, sizeof(addrs[i]));
        addrs[i].v4.sin_family = AF_INET;
        addrs[i].v4.sin_port = htons(51820);
        addrs[i].v4.sin_addr.s_addr = inet_addr("10.0.0.2");
    }
}

static void test_uniform_batch_is_one_run(void) {
    fill(32, 1400);
    ASSERT_EQ(gso_run_len(iov, addrs, 32), 32);
    /* A connected socket passes no addresses; the answer must not change. */
    ASSERT_EQ(gso_run_len(iov, NULL, 32), 32);
}

static void test_short_tail_rides_along(void) {
    /* The last chunk of a segmented buffer is allowed to be short — that is
     * exactly what a WireGuard keepalive at the end of a burst looks like. */
    fill(8, 1400);
    iov[7].iov_len = 32;
    ASSERT_EQ(gso_run_len(iov, addrs, 8), 8);
}

static void test_only_one_short_tail(void) {
    /* Two short packets: the first joins as the tail, the second cannot. */
    fill(8, 1400);
    iov[6].iov_len = 32;
    iov[7].iov_len = 32;
    ASSERT_EQ(gso_run_len(iov, addrs, 8), 7);
}

static void test_long_tail_ends_the_run(void) {
    /* A packet longer than gso_size cannot be a chunk of gso_size. */
    fill(8, 200);
    iov[5].iov_len = 1400;
    ASSERT_EQ(gso_run_len(iov, addrs, 8), 5);
}

static void test_keepalive_at_head_does_not_poison_the_batch(void) {
    /* The case that cost the offload in production: one small packet first.
     * The run is just that packet (so: no run), and the caller moves on to
     * the 31 full-size ones behind it. */
    fill(32, 1400);
    iov[0].iov_len = 32;
    ASSERT_EQ(gso_run_len(iov, addrs, 32), 0);
    ASSERT_EQ(gso_run_len(iov + 1, addrs + 1, 31), 31);
}

static void test_destination_change_ends_the_run(void) {
    /* Server mode: many clients on one socket. */
    fill(10, 1400);
    for (int i = 4; i < 10; i++)
        addrs[i].v4.sin_addr.s_addr = inet_addr("10.0.0.3");
    ASSERT_EQ(gso_run_len(iov, addrs, 10), 4);
    /* Same address, different port is still a different client. */
    fill(10, 1400);
    addrs[3].v4.sin_port = htons(51821);
    ASSERT_EQ(gso_run_len(iov, addrs, 10), 3);
    /* Without the address array the split must not happen — a connected
     * socket has one destination by construction. */
    fill(10, 1400);
    for (int i = 4; i < 10; i++)
        addrs[i].v4.sin_addr.s_addr = inet_addr("10.0.0.3");
    ASSERT_EQ(gso_run_len(iov, NULL, 10), 10);
}

static void test_segment_cap(void) {
    /* The kernel takes at most GSO_MAX_SEGMENTS chunks. */
    fill(MAXP, 100);
    ASSERT_EQ(gso_run_len(iov, addrs, MAXP), GSO_MAX_SEGMENTS);
    /* And the cap wins over the short-tail rule: a shorter packet sitting
     * exactly at the boundary must not become a 65th segment. */
    fill(MAXP, 100);
    iov[GSO_MAX_SEGMENTS].iov_len = 20;
    ASSERT_EQ(gso_run_len(iov, addrs, MAXP), GSO_MAX_SEGMENTS);
}

static void test_byte_cap(void) {
    /* 64 segments of 1400 is 89 600 bytes — past what one datagram may hold
     * before the kernel slices it, so the byte cap bites first. */
    fill(MAXP, 1400);
    int run = gso_run_len(iov, addrs, MAXP);
    ASSERT_EQ(run, (int)(GSO_MAX_BYTES / 1400));
    ASSERT((size_t)run * 1400 <= GSO_MAX_BYTES);
    ASSERT(run < GSO_MAX_SEGMENTS);
}

static void test_byte_cap_blocks_the_tail(void) {
    /* Room for 46 full segments (64 400 B) and 1107 B left: a 1200-byte tail
     * does not fit and must be left for the next send. */
    fill(MAXP, 1400);
    int full = (int)(GSO_MAX_BYTES / 1400);
    iov[full].iov_len = 1200;
    ASSERT_EQ(gso_run_len(iov, addrs, MAXP), full);
    /* …while a tail that does fit is taken. */
    fill(MAXP, 1400);
    iov[full].iov_len = GSO_MAX_BYTES - (size_t)full * 1400;
    ASSERT_EQ(gso_run_len(iov, addrs, MAXP), full + 1);
}

static void test_single_packet_is_not_a_run(void) {
    fill(1, 1400);
    ASSERT_EQ(gso_run_len(iov, addrs, 1), 0);
    ASSERT_EQ(gso_run_len(iov, addrs, 0), 0);
}

static void test_degenerate_lengths(void) {
    /* A zero-length first packet would mean gso_size 0, which the kernel
     * rejects outright. */
    fill(4, 0);
    ASSERT_EQ(gso_run_len(iov, addrs, 4), 0);
    /* A packet that already fills a datagram cannot be joined with anything. */
    fill(4, GSO_MAX_BYTES + 1);
    ASSERT_EQ(gso_run_len(iov, addrs, 4), 0);
}

static void test_pair_is_enough(void) {
    /* Two packets are already worth one syscall instead of two. */
    fill(2, 1400);
    ASSERT_EQ(gso_run_len(iov, addrs, 2), 2);
    fill(2, 1400);
    iov[1].iov_len = 32;
    ASSERT_EQ(gso_run_len(iov, addrs, 2), 2);
    fill(2, 32);
    iov[1].iov_len = 1400;
    ASSERT_EQ(gso_run_len(iov, addrs, 2), 0);
}

int main(void) {
    printf("=== gso tests ===\n");
    RUN_TEST(uniform_batch_is_one_run);
    RUN_TEST(short_tail_rides_along);
    RUN_TEST(only_one_short_tail);
    RUN_TEST(long_tail_ends_the_run);
    RUN_TEST(keepalive_at_head_does_not_poison_the_batch);
    RUN_TEST(destination_change_ends_the_run);
    RUN_TEST(segment_cap);
    RUN_TEST(byte_cap);
    RUN_TEST(byte_cap_blocks_the_tail);
    RUN_TEST(single_packet_is_not_a_run);
    RUN_TEST(degenerate_lengths);
    RUN_TEST(pair_is_enough);
    TEST_MAIN_END();
}
