#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

int vsprintf(char *buf, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int putchar(char c);
int printf(const char *s, ...);
int puts(const char *s);

void perror(const char *s);

#endif /* _STDIO_H */
