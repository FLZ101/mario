#include <stdio.h>
#include <stdarg.h>

#include <syscall.h>

_syscall1(int,putchar,char,c)

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

int puts(const char *s)
{
	return printf("%s\n", s);
}

_syscall2(int,rename,const char *,oldpath, const char *,newpath)
