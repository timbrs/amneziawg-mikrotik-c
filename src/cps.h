#ifndef AWG_CPS_H
#define AWG_CPS_H

#include "transform.h"
#include "fastrand.h"

/* Parse CPS template string. Returns 0 on success, -1 on error.
 * tmpl must be pre-allocated by caller. */
int cps_parse(const char *s, cps_template_t *tmpl);

/* Generate CPS packet from template. Returns packet length.
 * buf must be large enough (use cps_max_size to check).
 * <r>/<rc>/<rd> draw from rng, keyed by the caller from urandom: a seed
 * derived from counter would repeat the same bytes on every start. */
int cps_generate(const cps_template_t *tmpl, uint32_t counter, csprng_t *rng,
                 uint8_t *buf, int bufsize);

/* Max possible size of generated packet from template. */
int cps_max_size(const cps_template_t *tmpl);

/* Generate all configured CPS packets (I1-I5).
 * bufs[5] and lens[5] receive the packet data and lengths.
 * Returns number of packets generated. counter is incremented per packet. */
int cps_generate_all(cps_template_t *templates[5], uint32_t *counter,
                     csprng_t *rng, uint8_t bufs[][1500], int lens[]);

#endif
