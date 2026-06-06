#include "base64.h"

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, size_t inlen, unsigned char *out, size_t outmax) {
    /* Strip trailing padding */
    while (inlen > 0 && in[inlen-1] == '=') inlen--;
    if (inlen % 4 == 1) return -1;

    size_t outlen = (inlen * 3) / 4;
    if (outlen > outmax) return -1;

    size_t o = 0, i = 0;
    while (i + 4 <= inlen) {
        int a = b64_val(in[i]), b = b64_val(in[i+1]);
        int c = b64_val(in[i+2]), d = b64_val(in[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return -1;
        unsigned int v = ((unsigned)a << 18) | ((unsigned)b << 12) | ((unsigned)c << 6) | (unsigned)d;
        out[o++] = (v >> 16) & 0xFF;
        out[o++] = (v >> 8) & 0xFF;
        out[o++] = v & 0xFF;
        i += 4;
    }
    size_t rem = inlen - i;
    if (rem == 2) {
        int a = b64_val(in[i]), b = b64_val(in[i+1]);
        if (a < 0 || b < 0) return -1;
        out[o++] = (((unsigned)a << 18) | ((unsigned)b << 12)) >> 16;
    } else if (rem == 3) {
        int a = b64_val(in[i]), b = b64_val(in[i+1]), c = b64_val(in[i+2]);
        if (a < 0 || b < 0 || c < 0) return -1;
        unsigned int v = ((unsigned)a << 18) | ((unsigned)b << 12) | ((unsigned)c << 6);
        out[o++] = (v >> 16) & 0xFF;
        out[o++] = (v >> 8) & 0xFF;
    }
    return (int)o;
}
