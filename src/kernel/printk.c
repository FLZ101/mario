#include <misc.h>
#include <fs/tty/console.h>
#include <lib/stdarg.h>
#include <lib/stddef.h>
#include <multiboot.h>
#include <logbuf.h>

void console_reset(struct console *con);
void console_sync(struct console *con);
void console_write_char(struct console *con, unsigned char c);
void clear_screen(struct console *con);

static struct console *con;

void printk_init(struct multiboot_info *m)
{
	logbuf_init();

	con = get_fg_console();
	console_reset(con);

	unsigned char *s = (unsigned char *)m->boot_loader_name;
	if ((MB_FLAG_LOADER & m->flags) && (0x3f00 == *(unsigned short *)s)) {
		// if loaded by our bootloader
		console_sync(con);
	} else {
		clear_screen(con);
	}
}

static void putchar(char c)
{
	if (c == '\n')
		console_write_char(con, '\r');
	console_write_char(con, c);
}

/*
 * Append one character to a formatted string.
 *
 * @end is the one-past-last-formatted-character address for bounded output,
 * or NULL for the legacy unbounded printk/sprintk paths.  A bounded caller
 * passes buf + size - 1, reserving the final byte for the terminating NUL.
 */
static void sput_c(char **buf, char *end, char c)
{
	if (end && *buf >= end)
		return;
	*((*buf)++) = c;
}

static void sput_s(char **buf, char *end, char *str)
{
	char c;
	while ((c = *(str++)))
		sput_c(buf, end, c);
}

static void sput_x(char **buf, char *end, unsigned int n)
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
	sput_s(buf, end, s);
}

static void sput_u(char **buf, char *end, unsigned int n)
{
	int i, j;
	char s[11] = {0};

	if (n == 0) {
		sput_s(buf, end, "0");
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
	sput_s(buf, end, s + i + 1);
}

static void sput_d(char **buf, char *end, int n)
{
	if (0 <= n) {
		sput_u(buf, end, n);
	} else {
		sput_s(buf, end, "-");
		sput_u(buf, end, -n);
	}
}

/*
 * %u, %d, %x, %c, %s
 */
static int vformat(char *buf, char *end, const char *fmt, va_list ap)
{
	char *p0 = buf, *p1 = buf;
	char c;
	while ((c = *(fmt++))) {
		if (c == '%') {
			c = *(fmt++);
			switch (c) {
			case 'u':
				sput_u(&p1, end, va_arg(ap, unsigned int));
				break;
			case 'd':
				sput_d(&p1, end, va_arg(ap, int));
				break;
			case 'x':
				sput_x(&p1, end, va_arg(ap, unsigned int));
				break;
			case 'c':
				sput_c(&p1, end, va_arg(ap, char));
				break;
			case 's':
				sput_s(&p1, end, va_arg(ap, char *));
				break;
			default:
				break;
			}
			continue;
		}
		sput_c(&p1, end, c);
	}
	*p1 = '\0';
	return p1 - p0;
}

static int vsprintk(char *buf, const char *fmt, va_list ap)
{
	return vformat(buf, NULL, fmt, ap);
}

int sprintk(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsprintk(buf, fmt, ap);
	va_end(ap);
	return n;
}

/*
 * Bounded variants.  sprintkn()/vsprintkn() write at most size - 1 formatted
 * characters plus a NUL terminator, so they are safe for stack or heap
 * buffers whose capacity is known by the caller.
 */
int vsprintkn(char *buf, size_t size, const char *fmt, va_list ap)
{
	if (!buf || !size)
		return 0;
	return vformat(buf, buf + size - 1, fmt, ap);
}

int sprintkn(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsprintkn(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

int printk(const char *fmt, ...)
{
	static char buf[1024] = {0};
	char *p;
	int prio = LOG_INFO;
	va_list ap;
	int n;

	/*
	 * Accept the standard "<N>" priority prefix.  The prefix is stripped
	 * before formatting so it is stored and printed as a metadata priority,
	 * not as part of the visible message.  Malformed prefixes fall back to
	 * the default LOG_INFO priority.
	 */
	if (fmt[0] == '<') {
		const char *q = fmt + 1;
		int value = 0;

		while ('0' <= *q && *q <= '9') {
			value = value * 10 + (*q - '0');
			q++;
		}
		if (q != fmt + 1 && *q == '>') {
			prio = value & 7;
			fmt = q + 1;
		}
	}

	irq_save();

	va_start(ap, fmt);
	n = vsprintkn(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	logbuf_store(prio, buf, n);

	if (prio < logbuf_console_loglevel(0, 0)) {
		for (p = buf; *p; p++)
			putchar(*p);
	}

	irq_restore();
	return n;
}
