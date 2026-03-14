#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static int sput_c(char **buf, char *end, char c)
{
	if (*buf < end)
		*((*buf)++) = c;
	return 1;
}

static int sput_s(char **buf, char *end, char *str)
{
	int n = 0;
	char c;
	while ((c = *(str++)))
		n += sput_c(buf, end, c);
	return n;
}

static int sput_x(char **buf, char *end, unsigned int n)
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
	return sput_s(buf, end, s);
}

static int sput_u(char **buf, char *end, unsigned int n)
{
	int i, j;
	char s[11] = {0};

	if (n == 0)
		return sput_s(buf, end, "0");

	for (i = 9; i >= 0; i--) {
		if (n) {
			j = n;
			n = n/10;
			s[i] = j - n*10 + '0';
		} else {
			break;
		}
	}
	return sput_s(buf, end, s + i + 1);
}

static int sput_d(char **buf, char *end, int n)
{
	if (0 <= n)
		return sput_u(buf, end, n);
	else
		return sput_s(buf, end, "-") + sput_u(buf, end, -n);
}

/*
 * %u, %d, %x, %c, %s
 */
int vsnprintf(char *buf, size_t len, const char *fmt, va_list ap)
{
	if (!buf) {
		buf = (char *)&buf;
		len = 0;
	}

	int n = 0;
	char *p1 = buf;
	char *end = buf + len - 1;
	char c;
	while ((c = *(fmt++))) {
		if (c == '%') {
			c = *(fmt++);
			switch (c) {
			case 'u':
				n += sput_u(&p1, end, va_arg(ap, unsigned int));
				break;
			case 'd':
				n += sput_d(&p1, end, va_arg(ap, int));
				break;
			case 'x':
				n += sput_x(&p1, end, va_arg(ap, unsigned int));
				break;
			case 'c':
				n += sput_c(&p1, end, va_arg(ap, char));
				break;
			case 's':
				n += sput_s(&p1, end, va_arg(ap, char *));
				break;
			case '%':
				n += sput_c(&p1, end, '%');
				break;
			default:
				break;
			}
			continue;
		}
		n += sput_c(&p1, end, c);
	}

	if (p1 <= end)
		*p1 = '\0';
	return n;
}

int snprintf(char *buf, size_t len, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, len, fmt, ap);
	va_end(ap);
	return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
	return vsnprintf(buf, 4096, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsprintf(buf, fmt, ap);
	va_end(ap);
	return n;
}

#define USE_STATIC_PRINTF_BUF

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
	va_list ap_copy;
	size_t len;

	va_copy(ap_copy, ap);
	len = vsnprintf(NULL, 0, fmt, ap_copy);
	va_end(ap_copy);

#ifdef USE_STATIC_PRINTF_BUF
	#define LEN 1024
	static char buf[LEN] = {0};

	if (len + 1 > LEN)
		len = LEN - 1;
#else
	char *buf = malloc(len + 1);
	if (!buf)
		return -1;
#endif

	vsnprintf(buf, len + 1, fmt, ap);
	size_t n_write = fwrite(buf, 1, len, stream);

#ifdef USE_STATIC_PRINTF_BUF
	#undef LEN
#else
	free(buf);
#endif
	return n_write;
}

int vprintf(const char *fmt, va_list ap)
{
	return vfprintf(stdout, fmt, ap);
}

int fprintf(FILE *stream, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(stream, fmt, ap);
	va_end(ap);
	return n;
}

int printf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}
