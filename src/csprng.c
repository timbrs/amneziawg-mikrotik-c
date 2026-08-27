#include "csprng.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    if (done == len)
        return 0;
    done = 0;                     /* partial result is discarded, not reused */
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > 256)          /* getentropy() caps at 256 bytes per call */
            chunk = 256;
        if (getentropy(out + done, chunk) != 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        done += chunk;
    }
    if (done == len)
        return 0;
    done = 0;
#endif

    if (urandom_fd < 0 && !urandom_tried) {
        urandom_tried = 1;
        urandom_fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    }
    if (urandom_fd < 0)
        return -1;

    while (done < len) {
        ssize_t r = read(urandom_fd, out + done, len - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0)               /* EOF on /dev/urandom: the source is gone */
            return -1;
        done += (size_t)r;
    }
    return 0;
}

int csprng_init(void) {
    uint8_t probe[8];

    /* Verify we can actually get bytes: a container without /dev and without
     * getrandom(2) must fail loudly at startup instead of emitting predictable
     * padding for the lifetime of the tunnel. */
    return kernel_bytes(probe, sizeof(probe));
}

void csprng_bytes(uint8_t *out, size_t len) {
    /* Large request: straight from the kernel, no point buffering it. */
    if (len >= POOL_SIZE / 4) {
        if (kernel_bytes(out, len) != 0)
            goto no_entropy;
        return;
    }

    if (pool_left < len) {
        if (kernel_bytes(pool, POOL_SIZE) != 0)
            goto no_entropy;
        pool_left = POOL_SIZE;
    }

    {
        uint8_t *src = pool + (POOL_SIZE - pool_left);
        memcpy(out, src, len);
        /* Wipe what was handed out: a later read of this memory must not give
         * away padding and nonces that are already on the wire. */
        memset(src, 0, len);
        pool_left -= len;
    }
    return;

no_entropy:
    /* The source worked at startup (csprng_init) and has now disappeared, which
     * a healthy kernel does not do. Carrying on would put a predictable pattern
     * on the wire for as long as the tunnel lives, and that cannot be taken back
     * once a censor has seen it. Fail closed and let the supervisor restart us. */
    log_error("csprng: entropy source unavailable at runtime, aborting");
    abort();
}
