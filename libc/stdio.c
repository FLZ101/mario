#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>

static void sput_c(char **buf, char c)
{
	*((*buf)++) = c;
	**buf = '\0';
}

static void sput_s(char **buf, char *str)
{
	char c;
	while ((c = *(str++)))
		sput_c(buf, c);
}

static void sput_x(char **buf, unsigned int n)
{
	int i, j;
	char s[11] = {'0', 'x'};

	for (i = 9; i > 1; i--) {
		if (n) {
			j = n;
			n = n >> 4;
			j = j - (n << 4);
			s[i] = j + (j < 10 ? '0' : -10 + 'a');
		} else {
			s[i] = '0';
		}
	}
	sput_s(buf, s);
}

static void sput_u(char **buf, unsigned int n)
{
	int i, j;
	char s[11] = {0};

	if (n == 0) {
		sput_s(buf, "0");
		return;
	}

	for (i = 9; i >= 0; i--) {
		if (n) {
			j = n;
			n = n/10;
			s[i] = j - n*10 + '0';
		} else {
			break;
		}
	}
	sput_s(buf, s + i + 1);
}

static void sput_d(char **buf, int n)
{
	if (0 <= n) {
		sput_u(buf, n);
	} else {
		sput_s(buf, "-");
		sput_u(buf, -n);
	}
}

/*
 * %u, %d, %x, %c, %s
 */
int vsprintf(char *buf, const char *fmt, va_list ap)
{
	char *p0 = buf, *p1 = buf;
	char c;
	while ((c = *(fmt++))) {
		if (c == '%') {
			c = *(fmt++);
			switch (c) {
			case 'u':
				sput_u(&p1, va_arg(ap, unsigned int));
				break;
			case 'd':
				sput_d(&p1, va_arg(ap, int));
				break;
			case 'x':
				sput_x(&p1, va_arg(ap, unsigned int));
				break;
			case 'c':
				sput_c(&p1, va_arg(ap, char));
				break;
			case 's':
				sput_s(&p1, va_arg(ap, char *));
				break;
			case '%':
				sput_c(&p1, '%');
				break;
			default:
				break;
			}
			continue;
		}
		sput_c(&p1, c);
	}
	return p1 - p0;
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}

int printf(const char *fmt, ...)
{
	static char buf[1024] = {0};
	char *p = NULL;

	va_list ap;
	va_start(ap, fmt);
	int n = vsprintf(buf, fmt, ap);
	va_end(ap);

	for (p = buf; *p; p++)
		putchar(*p);
	return n;
}

int fputc(int c, FILE *stream)
{
	unsigned char buf = (unsigned char ) c;
	size_t n = fwrite(&buf, 1, 1, stream);
	if (!n)
		return EOF;
	return n;
}

int putchar(int c)
{
	return putc(c, stdout);
}

int fputs(const char *s, FILE *stream)
{
	int c;
	while ((c = *(s++))) {
		int n = fputc(c, stream);
		if (EOF == n)
			return EOF;
	}
	return 0;
}

int puts(const char *s)
{
	int n = fputs(s, stdout);
	if (EOF == n)
		return EOF;

	n = fputc('\n', stdout);
	if (EOF == n)
		return EOF;
	return 0;
}

_syscall2(int,rename,const char *,oldpath, const char *,newpath)

FILE *fopen(const char *pathname, const char *mode)
{
    int flags = 0;

    if (strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0) {
        flags = O_RDONLY;
    } else if (strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (strcmp(mode, "a") == 0 || strcmp(mode, "ab") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (strcmp(mode, "r+") == 0 || strcmp(mode, "rb+") == 0) {
        flags = O_RDWR;
    } else if (strcmp(mode, "w+") == 0 || strcmp(mode, "wb+") == 0) {
        flags = O_RDWR | O_CREAT | O_TRUNC;
    } else if (strcmp(mode, "a+") == 0 || strcmp(mode, "ab+") == 0) {
        flags = O_RDWR | O_CREAT | O_APPEND;
    } else {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(pathname, flags);
    if (fd == -1) {
        return NULL;
    }

    FILE *f = (FILE *) malloc(sizeof(FILE));
    if (!f) {
        close(fd);
        return NULL;
    }

    f->fd = fd;
    return f;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;

    int ret = 0;
    if (close(stream->fd) == -1) {
        ret = EOF;
    }
    free(stream);
    return ret;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!stream || stream->fd < 0) return 0;

    size_t left = size * nmemb;
	ssize_t n = 0;
	while (left > 0) {
		// if cache is empty, load
		if (stream->p >= stream->p_end) {
			int n_read = read(stream->fd, stream->cache, FILE_CACHE_SIZE);
			if (n_read == -1) // error
				break;
			if (n_read == 0) // EOF
				break;
			stream->p = 0;
			stream->p_end = n_read;
		}

		while (left > 0 && stream->p < stream->p_end) {
			*(char *)ptr++ = stream->cache[stream->p++];
			--left;
			++n;
		}
	}

    return (size_t) n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!stream || stream->fd < 0) return 0;

    size_t total = size * nmemb;
    if (total == 0) return 0;

    ssize_t n = write(stream->fd, ptr, total);
    if (n == -1) return 0;

    return (size_t) n / size;
}

static FILE stdin_file = { .fd = 0, .cache = { 0 }, .p = 0, .p_end = 0 };
static FILE stdout_file = { .fd = 1, .cache = { 0 }, .p = 0, .p_end = 0 };
static FILE stderr_file = { .fd = 2, .cache = { 0 }, .p = 0, .p_end = 0 };

FILE *stdin = &stdin_file;
FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    // 1. Validation
    if (!lineptr || !n || !stream) {
        errno = EINVAL;
        return -1;
    }

    // 2. Initialize buffer if needed
    if (*lineptr == NULL || *n == 0) {
        *n = 128; // Default initial size
        *lineptr = malloc(*n);
        if (*lineptr == NULL) {
            return -1; // Memory allocation failed
        }
    }

    size_t len = 0;
    char c;

    // 3. Read loop
    while (1) {
        // Read one character
        if (fread(&c, 1, 1, stream) == 0) {
            // EOF or Error
            if (len == 0) {
                return EOF; // No data read, true EOF
            }
            break; // Data was read, but now EOF. Finish the line.
        }

        // 4. Check if we need to resize the buffer
        // We need space for the new char + the null terminator '\0'
        if (len + 1 >= *n) {
            size_t new_size = *n * 2; // Double the size
            char *new_ptr = realloc(*lineptr, new_size);

            if (new_ptr == NULL) {
                // Reallocation failed.
                // We stop here, null-terminate what we have, and return.
                (*lineptr)[len] = '\0';
                return (ssize_t)len;
            }

            *lineptr = new_ptr;
            *n = new_size;
        }

        // 5. Store character
        (*lineptr)[len++] = c;

        // 6. Stop if newline
        if (c == '\n') {
            break;
        }
    }

    // 7. Null-terminate the string
    (*lineptr)[len] = '\0';

    return (ssize_t)len;
}
