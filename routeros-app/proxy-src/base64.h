#ifndef AWG_BASE64_H
#define AWG_BASE64_H

#include <stddef.h>

/* Decode standard base64 into out. Returns decoded length, or -1 on error. */
int base64_decode(const char *in, size_t inlen, unsigned char *out, size_t outmax);

#endif
