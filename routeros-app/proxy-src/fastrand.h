#ifndef AWG_FASTRAND_H
#define AWG_FASTRAND_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t s;
} fastrand_t;

void fastrand_init(fastrand_t *r, uint64_t seed);
uint64_t fastrand_u64(fastrand_t *r);
int fastrand_intn(fastrand_t *r, int n);
void fastrand_fill(fastrand_t *r, void *buf, size_t len);

#endif
