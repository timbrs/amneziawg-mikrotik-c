/* The CSPRNG behind everything that goes on the wire raw: S padding, junk
 * bodies, CPS random segments. These tests do not judge the kernel's entropy —
 * they pin the properties of this wrapper that a caller depends on: it fills
 * exactly what it was asked for, it does not repeat, and it keeps working
 * across the internal pool boundary and for requests larger than the pool. */
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "test.h"
#include "csprng.h"

#define POOL_SIZE 4096            /* mirrors csprng.c */

static void test_init_succeeds(void) {
    ASSERT_EQ(csprng_init(), 0);
}

/* Two draws of any size must differ. 32 bytes: a collision on healthy entropy
 * is not something that happens before the sun burns out. */
static void test_draws_differ(void) {
    uint8_t a[32], b[32];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    csprng_bytes(a, sizeof(a));
    csprng_bytes(b, sizeof(b));
    ASSERT(memcmp(a, b, sizeof(a)) != 0);
}

/* The one property the old fastrand path failed: the second eight bytes must
 * not be a step of xorshift64 applied to the first eight. */
static void test_not_an_xorshift_chain(void) {
    for (int i = 0; i < 64; i++) {
        uint8_t buf[16];
        csprng_bytes(buf, sizeof(buf));
        uint64_t v;
        memcpy(&v, buf, 8);
        v ^= v << 13;
        v ^= v >> 7;
        v ^= v << 17;
        ASSERT(memcmp(buf + 8, &v, 8) != 0);
    }
}

/* Exactly len bytes are written and nothing beyond them is touched. */
static void test_writes_exact_length(void) {
    for (int len = 1; len <= 64; len++) {
        uint8_t buf[128];
        memset(buf, 0x5A, sizeof(buf));
        csprng_bytes(buf, (size_t)len);
        for (int i = len; i < 128; i++)
            ASSERT_EQ(buf[i], 0x5A);
    }
}

/* Requests keep working across the internal pool refill: 12 bytes at a time is
 * the header-protection nonce size, and the pool boundary must not repeat or
 * short-fill anything. Collect a window of draws around the refill and check
 * they are all distinct. */
static void test_crosses_pool_boundary(void) {
    enum { N = POOL_SIZE / 12 + 8, LEN = 12 };
    static uint8_t seen[N][LEN];

    for (int i = 0; i < N; i++) {
        memset(seen[i], 0, LEN);
        csprng_bytes(seen[i], LEN);
    }
    for (int i = 0; i < N; i++) {
        int all_zero = 1;
        for (int k = 0; k < LEN; k++)
            if (seen[i][k]) { all_zero = 0; break; }
        ASSERT(!all_zero);
        for (int j = i + 1; j < N; j++)
            ASSERT(memcmp(seen[i], seen[j], LEN) != 0);
    }
}

/* A request larger than the pool bypasses it and must still be filled whole. */
static void test_large_request(void) {
    static uint8_t big[POOL_SIZE * 2];
    memset(big, 0, sizeof(big));
    csprng_bytes(big, sizeof(big));

    /* No 64-byte run of zeros: a partial fill would leave one. */
    int zero_run = 0;
    for (size_t i = 0; i < sizeof(big); i++) {
        zero_run = big[i] ? 0 : zero_run + 1;
        ASSERT(zero_run < 64);
    }
}

/* The pool is per-thread, so two threads must not hand out the same bytes. */
static void *thread_draw(void *arg) {
    csprng_bytes((uint8_t *)arg, 64);
    return NULL;
}

static void test_threads_get_distinct_bytes(void) {
    uint8_t a[64], b[64];
    pthread_t ta, tb;
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    ASSERT_EQ(pthread_create(&ta, NULL, thread_draw, a), 0);
    ASSERT_EQ(pthread_create(&tb, NULL, thread_draw, b), 0);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    ASSERT(memcmp(a, b, sizeof(a)) != 0);
}

int main(void) {
    fprintf(stderr, "=== csprng tests ===\n");
    RUN_TEST(init_succeeds);
    RUN_TEST(draws_differ);
    RUN_TEST(not_an_xorshift_chain);
    RUN_TEST(writes_exact_length);
    RUN_TEST(crosses_pool_boundary);
    RUN_TEST(large_request);
    RUN_TEST(threads_get_distinct_bytes);
    TEST_MAIN_END();
}
