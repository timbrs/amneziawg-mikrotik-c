#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>

void app_log(const char *level, const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "%s: ", level);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}
