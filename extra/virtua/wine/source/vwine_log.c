#include "vwine/vwine_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

void vwine_logf(const char* fmt, ...)
{
    char line[256];
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (n <= 0) return;
    size_t len = (size_t)n;
    if (len >= sizeof(line)) len = sizeof(line) - 1u;
    (void)write(2, line, len);
}
