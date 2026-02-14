#include <misc.h>
#include <fs/tty/console.h>
#include <lib/stdarg.h>
#include <lib/stddef.h>
#include <multiboot.h>

void console_reset(struct console *con);
void console_sync(struct console *con);
void console_write_char(struct console *con, unsigned char c);
void clear_screen(struct console *con);

static struct console *con;

void __tinit printk_init(struct multiboot_info *m)
{
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
	console_write_char(con, c);
}

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
static int vsprintk(char *buf, const char *fmt, va_list ap)
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

int sprintk(char *buf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsprintk(buf, fmt, ap);
	va_end(ap);
	return n;
}

int printk(const char *fmt, ...)
{
	irq_save();

	static char buf[1024] = {0};
	char *p = NULL;

	va_list ap;
	va_start(ap, fmt);
	int n = vsprintk(buf, fmt, ap);
	va_end(ap);


	for (p = buf; *p; p++)
		putchar(*p);

	irq_restore();
	return n;
}
