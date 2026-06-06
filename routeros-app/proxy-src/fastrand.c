#include "fastrand.h"
#include <string.h>

void fastrand_init(fastrand_t *r, uint64_t seed) {
    r->s = seed ? seed : 1;
}

uint64_t fastrand_u64(fastrand_t *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}

int fastrand_intn(fastrand_t *r, int n) {
    return (int)(fastrand_u64(r) % (uint64_t)n);
}

void fastrand_fill(fastrand_t *r, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t i;
    for (i = 0; i + 8 <= len; i += 8) {
        uint64_t v = fastrand_u64(r);
        memcpy(p + i, &v, 8);
    }
    if (i < len) {
        uint64_t v = fastrand_u64(r);
        memcpy(p + i, &v, len - i);
    }
}
