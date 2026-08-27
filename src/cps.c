#include "cps.h"
#include "csprng.h"
#include <string.h>
#include <time.h>

/* <rc N> is letters only upstream (device/obf_randchars.go: chars52) — digits
 * here would give the I-packets a different byte profile than a real client. */
static const char chars52[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
#define CHARS52_LEN 52

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_int(const char *s, int len) {
    if (len <= 0) return -1;
    int v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        if (v > 100000) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static int skip_spaces(const char *s, int i, int len) {
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

int cps_parse(const char *s, cps_template_t *tmpl) {
    memset(tmpl, 0, sizeof(*tmpl));
    int slen = 0;
    while (s[slen]) slen++;

    int i = 0;
    while (i < slen) {
        /* Skip whitespace */
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r') { i++; continue; }
        if (s[i] != '<') return -1;

        /* Find closing '>' */
        int end = -1;
        for (int j = i + 1; j < slen; j++) {
            if (s[j] == '>') { end = j; break; }
        }
        if (end < 0) return -1;

        if (tmpl->nseg >= CPS_MAX_SEGMENTS) return -1;

        int inner_start = i + 1;
        int inner_len = end - inner_start;
        if (inner_len <= 0) return -1;

        cps_segment_t *seg = &tmpl->segs[tmpl->nseg];
        char kind = s[inner_start];

        switch (kind) {
        case 'b': {
            /* <b 0xHEXDATA> */
            int p = skip_spaces(s, inner_start + 1, end);
            if (p + 2 >= end || s[p] != '0' || (s[p+1] != 'x' && s[p+1] != 'X'))
                return -1;
            p += 2;
            int hexlen = end - p;
            if (hexlen % 2 != 0) return -1;
            int nbytes = hexlen / 2;
            if (tmpl->static_used + nbytes > CPS_MAX_STATIC) return -1;
            seg->kind = CPS_STATIC;
            seg->data_off = tmpl->static_used;
            seg->data_len = nbytes;
            for (int j = 0; j < hexlen; j += 2) {
                int hi = hex_val(s[p + j]);
                int lo = hex_val(s[p + j + 1]);
                if (hi < 0 || lo < 0) return -1;
                tmpl->static_data[tmpl->static_used++] = (uint8_t)((hi << 4) | lo);
            }
            break;
        }
        case 'r': {
            /* <r SIZE>, <rc SIZE>, <rd SIZE> */
            if (inner_len > 1 && s[inner_start + 1] == 'c') {
                int p = skip_spaces(s, inner_start + 2, end);
                int size = parse_int(s + p, end - p);
                if (size <= 0) return -1;
                seg->kind = CPS_RANDOM_CHARS;
                seg->size = size;
            } else if (inner_len > 1 && s[inner_start + 1] == 'd') {
                int p = skip_spaces(s, inner_start + 2, end);
                int size = parse_int(s + p, end - p);
                if (size <= 0) return -1;
                seg->kind = CPS_RANDOM_DIGITS;
                seg->size = size;
            } else {
                int p = skip_spaces(s, inner_start + 1, end);
                int size = parse_int(s + p, end - p);
                if (size <= 0) return -1;
                seg->kind = CPS_RANDOM;
                seg->size = size;
            }
            break;
        }
        case 't':
            seg->kind = CPS_TIMESTAMP;
            break;
        case 'c':
            seg->kind = CPS_COUNTER;
            break;
        case 'd':
            /* AWG 3.0 tags from device/obf.go. I-packets are generated with an
             * empty source, so <d> and <ds> contribute nothing — skip them
             * rather than reject the template. <dz N> is different: its
             * ObfuscatedLen ignores the source and it emits N bytes holding the
             * source length, i.e. N zero bytes. */
            if (inner_len > 1 && s[inner_start + 1] == 'z') {
                int p = skip_spaces(s, inner_start + 2, end);
                int size = parse_int(s + p, end - p);
                if (size <= 0) return -1;
                seg->kind = CPS_ZEROS;
                seg->size = size;
                break;
            }
            i = end + 1;
            continue;
        default:
            return -1;
        }

        tmpl->nseg++;
        i = end + 1;
    }

    return (tmpl->nseg > 0) ? 0 : -1;
}

int cps_max_size(const cps_template_t *tmpl) {
    int total = 0;
    for (int i = 0; i < tmpl->nseg; i++) {
        const cps_segment_t *seg = &tmpl->segs[i];
        switch (seg->kind) {
        case CPS_STATIC: total += seg->data_len; break;
        case CPS_RANDOM: case CPS_RANDOM_CHARS: case CPS_RANDOM_DIGITS:
        case CPS_ZEROS:
            total += seg->size; break;
        case CPS_TIMESTAMP: case CPS_COUNTER:
            total += 4; break;
        }
    }
    return total;
}

int cps_generate(const cps_template_t *tmpl, uint32_t counter, uint8_t *buf, int bufsize) {
    int off = 0;

    /* The random segments go on the wire raw, so they come from the CSPRNG —
     * as in the reference (device/obf_rand.go, obf_randchars.go: rand.Read).
     * They used to be xorshift64 seeded with counter ^ constant, which made the
     * bytes of the Nth CPS packet identical on every run of every instance. */
    for (int i = 0; i < tmpl->nseg; i++) {
        const cps_segment_t *seg = &tmpl->segs[i];
        switch (seg->kind) {
        case CPS_STATIC:
            if (off + seg->data_len > bufsize) return off;
            memcpy(buf + off, tmpl->static_data + seg->data_off, seg->data_len);
            off += seg->data_len;
            break;
        case CPS_RANDOM:
            if (off + seg->size > bufsize) return off;
            csprng_bytes(buf + off, (size_t)seg->size);
            off += seg->size;
            break;
        case CPS_RANDOM_CHARS:
            if (off + seg->size > bufsize) return off;
            csprng_bytes(buf + off, (size_t)seg->size);
            for (int j = 0; j < seg->size; j++)
                buf[off + j] = chars52[buf[off + j] % CHARS52_LEN];
            off += seg->size;
            break;
        case CPS_ZEROS:
            if (off + seg->size > bufsize) return off;
            memset(buf + off, 0, seg->size);
            off += seg->size;
            break;
        case CPS_RANDOM_DIGITS:
            if (off + seg->size > bufsize) return off;
            csprng_bytes(buf + off, (size_t)seg->size);
            for (int j = 0; j < seg->size; j++)
                buf[off + j] = (uint8_t)('0' + buf[off + j] % 10);
            off += seg->size;
            break;
        case CPS_TIMESTAMP: {
            if (off + 4 > bufsize) return off;
            /* Big-endian, as in device/obf_timestamp.go */
            uint32_t ts = (uint32_t)time(NULL);
            buf[off]     = (uint8_t)(ts >> 24);
            buf[off + 1] = (uint8_t)(ts >> 16);
            buf[off + 2] = (uint8_t)(ts >> 8);
            buf[off + 3] = (uint8_t)ts;
            off += 4;
            break;
        }
        case CPS_COUNTER:
            if (off + 4 > bufsize) return off;
            memcpy(buf + off, &counter, 4);
            off += 4;
            break;
        }
    }
    return off;
}

int cps_generate_all(cps_template_t *templates[5], uint32_t *counter,
                     uint8_t bufs[][1500], int lens[]) {
    int count = 0;
    for (int i = 0; i < 5; i++) {
        if (!templates[i]) continue;
        lens[count] = cps_generate(templates[i], *counter, bufs[count], 1500);
        (*counter)++;
        count++;
    }
    return count;
}
