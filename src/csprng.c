#include "csprng.h"

#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/random.h>
#endif

#define POOL_SIZE 4096

/* Everything per-thread: no locks on the hot path, and two file descriptors at
 * worst (the proxy runs two packet threads). */
static __thread uint8_t pool[POOL_SIZE];
static __thread size_t pool_left;    /* unread bytes at the tail of pool[] */
static __thread int urandom_fd = -1;
static __thread int urandom_tried;

/* Raw kernel entropy. Returns 0 on success, -1 if nothing could be read. */
static int kernel_bytes(uint8_t *out, size_t len) {
    size_t done = 0;

#if defined(__linux__) && defined(SYS_getrandom)
    /* getrandom(2) needs no file descriptor (Linux 3.17+); older kernels return
     * ENOSYS and we fall through to /dev/urandom. */
    while (done < len) {
        long r = syscall(SYS_getrandom, out + done, len - done, 0);
        if (r <= 0)
            break;
        done += (size_t)r;
    }
    if (done == len)
        return 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > 256)          /* getentropy() caps at 256 bytes per call */
            chunk = 256;
        if (getentropy(out + done, chunk) != 0)
            break;
        done += chunk;
    }
    if (done == len)
        return 0;
#endif

    if (urandom_fd < 0 && !urandom_tried) {
        urandom_tried = 1;
        urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    }
    if (urandom_fd < 0)
        return -1;

    while (done < len) {
        ssize_t r = read(urandom_fd, out + done, len - done);
        if (r <= 0)
            return -1;
        done += (size_t)r;
    }
    return 0;
}

int csprng_init(void) {
    uint8_t probe[8];

    /* Verify we can actually get bytes: a container without /dev and without
     * getrandom(2) must fail loudly instead of emitting predictable padding. */
    return kernel_bytes(probe, sizeof(probe));
}

void csprng_bytes(uint8_t *out, size_t len) {
    /* Large request: straight from the kernel, no point buffering it. */
    if (len >= POOL_SIZE / 4) {
        if (kernel_bytes(out, len) != 0)
            memset(out, 0, len);
        return;
    }

    if (pool_left < len) {
        if (kernel_bytes(pool, POOL_SIZE) != 0) {
            /* Entropy source vanished at runtime. Zero padding makes the packet
             * useless to the peer, which is recoverable; predictable padding
             * would be a permanent fingerprint, which is not. */
            memset(out, 0, len);
            return;
        }
        pool_left = POOL_SIZE;
    }

    memcpy(out, pool + (POOL_SIZE - pool_left), len);
    pool_left -= len;
}
