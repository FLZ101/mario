#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

#define FILE_CACHE_SIZE 512

enum FileWorkMode {
	READ,
	WRITE,
	READ_ONLY,
	WRITE_ONLY,
};

enum FileBufferMode {
	UNBUFFERED,
	FULLY_BUFFERED,
};

typedef struct {
	int fd;
	char cache[FILE_CACHE_SIZE];
	int p, p_end;
	enum FileWorkMode work_mode;
	enum FileBufferMode buffer_mode;
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

int vsnprintf(char *buf, size_t len, const char *fmt, va_list ap);
int snprintf(char *buf, size_t len, const char *fmt, ...);

int vsprintf(char *buf, const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);

int vfprintf(FILE *stream, const char *fmt, va_list ap);
int fprintf(FILE *stream, const char *fmt, ...);

int vprintf(const char *fmt, va_list ap);
int printf(const char *fmt, ...);

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

#ifndef SEEK_SET
#define SEEK_SET 0
#endif

#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif

#ifndef SEEK_END
#define SEEK_END 2
#endif

int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);

void rewind(FILE *stream);

int fflush(FILE *stream);

#endif /* _STDIO_H */
