#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

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
void vsprintf(char *__buf, const char *fmt, va_list ap)
{
	char c, **buf = &__buf;
	while ((c = *(fmt++))) {
		if (c == '%') {
			c = *(fmt++);
			switch (c) {
			case 'u':
				sput_u(buf, va_arg(ap, unsigned int));
				break;
			case 'd':
				sput_d(buf, va_arg(ap, int));
				break;
			case 'x':
				sput_x(buf, va_arg(ap, unsigned int));
				break;
			case 'c':
				sput_c(buf, va_arg(ap, char));
				break;
			case 's':
				sput_s(buf, va_arg(ap, char *));
				break;
			default:
				break;
			}
			continue;
		}
		sput_c(buf, c);
	}
}

static char print_buf[1024] = {0};

int printf(const char *fmt, ...)
{
	char *p = NULL;

	va_list ap;
	va_start(ap, fmt);
	vsprintf(print_buf, fmt, ap);
	va_end(ap);

	for (p = print_buf; *p; p++)
		putchar(*p);
	return 0;
}
