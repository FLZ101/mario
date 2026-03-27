#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

char *Sprintf(const char *fmt, ...)
{
    size_t len;
    va_list ap;

    va_start(ap, fmt);
    len = vsprintf(NULL, fmt, ap);
    va_end(ap);

    char *buf = malloc(len + 1);
    if (!buf)
        return NULL;

    va_start(ap, fmt);
    (void) vsprintf(buf, fmt, ap);
    va_end(ap);

    return buf;
}
