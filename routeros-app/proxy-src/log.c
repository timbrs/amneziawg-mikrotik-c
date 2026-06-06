#include "log.h"
#include <unistd.h>
#include <string.h>

int g_log_level = LOG_INFO;

static inline int slen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (int)(p - s);
}

void log_msg(const char *prefix, const char *msg) {
    char buf[512];
    int n = 0;
    int pl = slen(prefix);
    if (pl > 256) pl = 256;
    memcpy(buf, prefix, pl);
    n = pl;
    int ml = slen(msg);
    if (n + ml > 510) ml = 510 - n;
    memcpy(buf + n, msg, ml);
    n += ml;
    buf[n++] = '\n';
    write(STDERR_FILENO, buf, n);
}

void log_msg2(const char *prefix, const char *a, const char *b) {
    char buf[512];
    int n = 0;
    int pl = slen(prefix);
    if (pl > 256) pl = 256;
    memcpy(buf, prefix, pl);
    n = pl;
    int al = slen(a);
    if (n + al > 500) al = 500 - n;
    memcpy(buf + n, a, al);
    n += al;
    int bl = slen(b);
    if (n + bl > 510) bl = 510 - n;
    memcpy(buf + n, b, bl);
    n += bl;
    buf[n++] = '\n';
    write(STDERR_FILENO, buf, n);
}

void log_msg3(const char *prefix, const char *a, const char *b, const char *c) {
    char buf[512];
    int n = 0;
    int pl = slen(prefix);
    if (pl > 200) pl = 200;
    memcpy(buf, prefix, pl); n = pl;
    int al = slen(a); if (n+al > 400) al = 400-n; memcpy(buf+n, a, al); n += al;
    int bl = slen(b); if (n+bl > 480) bl = 480-n; memcpy(buf+n, b, bl); n += bl;
    int cl = slen(c); if (n+cl > 510) cl = 510-n; memcpy(buf+n, c, cl); n += cl;
    buf[n++] = '\n';
    write(STDERR_FILENO, buf, n);
}

void log_msgn(const char *prefix, const char **parts, int count) {
    char buf[512];
    int n = 0;
    int pl = slen(prefix);
    if (pl > 200) pl = 200;
    memcpy(buf, prefix, pl);
    n = pl;
    for (int i = 0; i < count && n < 510; i++) {
        int l = slen(parts[i]);
        if (n + l > 510) l = 510 - n;
        memcpy(buf + n, parts[i], l);
        n += l;
    }
    buf[n++] = '\n';
    write(STDERR_FILENO, buf, n);
}

char *u32_to_str(char *buf, unsigned int v) {
    /* buf must be at least 12 bytes. Returns pointer within buf. */
    char *p = buf + 11;
    *p = '\0';
    if (v == 0) { *--p = '0'; return p; }
    while (v > 0) {
        *--p = '0' + (v % 10);
        v /= 10;
    }
    return p;
}
