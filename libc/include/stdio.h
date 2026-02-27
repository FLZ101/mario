#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

typedef struct {
	int fd;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int vsprintf(char *buf, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int printf(const char *s, ...);

int fputc(int c, FILE *stream);

#define putc(c, stream) fputc((c), (stream))

int putchar(int c);

int fputs(const char *s, FILE *stream);
int puts(const char *s);

int vsscanf(const char *s, const char *fmt, va_list ap);
int sscanf(const char *s, const char *fmt, ...);

void perror(const char *s);

#define STRINGIFY(x)	#x
#define TO_STRING(x)	STRINGIFY(x)

#define _perror()	perror(__FILE__ ":" TO_STRING(__LINE__))

int rename(const char *oldpath, const char *newpath);

#define EOF -1

FILE *fopen(const char *pathname, const char *mode);
int fclose(FILE *stream);

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

ssize_t getline(char **lineptr, size_t *n, FILE *stream);

#endif /* _STDIO_H */
