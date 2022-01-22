#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

extern void vsprintf(char *buf, const char *fmt, va_list ap);
extern int putchar(char c);
extern int printf(const char *s, ...);

static inline int puts(const char *s)
{
	return printf("%s\n", s);
}

#endif /* _STDIO_H */
