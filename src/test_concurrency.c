/*
 * Unit tests for the proxy's cross-thread concurrency primitives
 * (src/concurrency.h):
 *
 *   1. client_slot_t — single-writer seqlock publishing the client
 *      sockaddr so the s2c reader never observes a torn address while
 *      c2s rewrites it on a client port roam.
 *   2. fd_retire_t — one-generation deferred close of the remote fd so
 *      c2s can never send on a descriptor that do_reconnect() closed (and
 *      the kernel may have reused) mid-iteration.
 *
 * Portable: C11 atomics + pthreads + POSIX fcntl/close. Runs on the dev
 * mac and on the Linux build host. No proxy.h (avoids Linux-only mmsghdr).
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "test.h"
#include "concurrency.h"

/* ---- client_slot_t: seqlock ---- */

static struct sockaddr_in mkaddr(uint32_t v) {
    /* Self-consistent encoding: the port mirrors the low 16 bits of the
     * address, so any torn read (new addr + old port, or vice versa) is
     * detectable by the reader. */
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = v;
    a.sin_port = (uint16_t)(v & 0xFFFF);
    return a;
}

static void test_client_slot_basic(void) {
    client_slot_t cs;
    client_slot_init(&cs);

    struct sockaddr_in out;
    /* No write yet: read reports "no value". */
    ASSERT(client_slot_read(&cs, &out) == 0);

    struct sockaddr_in in = mkaddr(0xABCD1234);
    client_slot_write(&cs, &in);

    ASSERT(client_slot_read(&cs, &out) == 1);
    ASSERT(out.sin_addr.s_addr == in.sin_addr.s_addr);
    ASSERT(out.sin_port == in.sin_port);
}

#define SEQ_VALUE_A 0x11111111u
#define SEQ_VALUE_B 0x22222222u
#define SEQ_READS   2000000

typedef struct {
    client_slot_t *cs;
    _Atomic int stop;
    _Atomic long torn;   /* count of inconsistent reads observed */
    _Atomic long reads;  /* count of successful (consistent) reads */
} seq_reader_t;

static void *seq_reader(void *arg) {
    seq_reader_t *r = (seq_reader_t *)arg;
    struct sockaddr_in out;
    long reads = 0, torn = 0;
    while (!atomic_load_explicit(&r->stop, memory_order_relaxed)) {
        if (!client_slot_read(r->cs, &out)) continue;
        /* Invariant: port mirrors low 16 bits of address. A torn read
         * (half from value A, half from value B) breaks it. */
        if (out.sin_port != (uint16_t)(out.sin_addr.s_addr & 0xFFFF))
            torn++;
        reads++;
    }
    atomic_store_explicit(&r->reads, reads, memory_order_relaxed);
    atomic_store_explicit(&r->torn, torn, memory_order_relaxed);
    return NULL;
}

static void test_client_slot_no_torn_read(void) {
    client_slot_t cs;
    client_slot_init(&cs);
    struct sockaddr_in init = mkaddr(SEQ_VALUE_A);
    client_slot_write(&cs, &init);

    seq_reader_t r;
    memset(&r, 0, sizeof(r));
    r.cs = &cs;
    pthread_t t;
    pthread_create(&t, NULL, seq_reader, &r);

    /* Writer: flip between two self-consistent values many times. */
    for (long i = 0; i < SEQ_READS; i++) {
        struct sockaddr_in a = mkaddr((i & 1) ? SEQ_VALUE_B : SEQ_VALUE_A);
        client_slot_write(&cs, &a);
    }
    atomic_store_explicit(&r.stop, 1, memory_order_relaxed);
    pthread_join(t, NULL);

    long reads = atomic_load_explicit(&r.reads, memory_order_relaxed);
    long torn = atomic_load_explicit(&r.torn, memory_order_relaxed);
    fprintf(stderr, "          (consistent reads=%ld, torn=%ld)\n", reads, torn);
    ASSERT(reads > 0);   /* reader actually ran concurrently */
    ASSERT(torn == 0);   /* seqlock guarantees no torn read is ever returned */
}

/* ---- fd_retire_t: one-generation deferred close ---- */

static int fd_is_open(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

/* A throwaway but valid fd. */
static int open_fd(void) {
    int fd = dup(STDIN_FILENO);
    return fd;
}

static void test_fd_retire_grace(void) {
    fd_retire_t r;
    fd_retire_init(&r);

    int a = open_fd(); ASSERT(a >= 0);
    int b = open_fd(); ASSERT(b >= 0);
    int c = open_fd(); ASSERT(c >= 0);
    ASSERT(a != b && b != c && a != c);

    /* Retire A: nothing closed yet — A must remain valid for any thread
     * still holding it from before the swap. */
    fd_retire_push(&r, a);
    ASSERT(fd_is_open(a));

    /* Retire B: A is now one generation old and gets closed; B survives. */
    fd_retire_push(&r, b);
    ASSERT(!fd_is_open(a));
    ASSERT(fd_is_open(b));

    /* Retire C: B closed, C survives. */
    fd_retire_push(&r, c);
    ASSERT(!fd_is_open(b));
    ASSERT(fd_is_open(c));

    /* Drain on shutdown: the last retained fd is closed. */
    fd_retire_drain(&r);
    ASSERT(!fd_is_open(c));
}

static void test_fd_retire_ignores_negative(void) {
    fd_retire_t r;
    fd_retire_init(&r);

    int a = open_fd(); ASSERT(a >= 0);
    fd_retire_push(&r, a);
    /* A negative (absent) fd must not disturb the retained generation. */
    fd_retire_push(&r, -1);
    ASSERT(fd_is_open(a));   /* still retained, not closed by the -1 push */

    fd_retire_drain(&r);
    ASSERT(!fd_is_open(a));
}

int main(void) {
    fprintf(stderr, "=== concurrency tests ===\n");
    RUN_TEST(client_slot_basic);
    RUN_TEST(client_slot_no_torn_read);
    RUN_TEST(fd_retire_grace);
    RUN_TEST(fd_retire_ignores_negative);
    TEST_MAIN_END();
}
