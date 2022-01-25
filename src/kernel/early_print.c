#include <misc.h>
#include <io.h>

#include <lib/stdarg.h>
#include <lib/string.h>

#define TEXT (0xb8000 + KERNEL_BASE)

#define COLOR 0x07

#define MAKEC(c) (COLOR << 8 | (c))

#define SPACE MAKEC(' ')

int pos_x __dinit = 0;
int pos_y __dinit = 0;

void __tinit move_cursor(void)
{
	unsigned int pos = 80*pos_y + pos_x;

	outb(0x3d4, 14);
	outb(0x3d5, pos >> 8);
	outb(0x3d4, 15);
	outb(0x3d5, pos);
}

void __tinit set_pos(int x, int y)
{
	if (0 <= x && x < 25)
		pos_x = x;
	if (0 <= y && y < 80)
		pos_y = y;

	move_cursor();
}

void __tinit cls(void)
{
	memsetw((void *)TEXT, SPACE, 80*25);
	set_pos(0, 0);
}

void __tinit scroll_one_line(void)
{
	memmove((void *)TEXT, (void *)(TEXT + 80*2), 80*2*24);
	memsetw((void *)(TEXT + 80*2*24), SPACE, 80);
}

void __tinit write_c(unsigned char c)
{
	if (c == '\t') {
		pos_x += 8;
		pos_x &= ~7;
	} else if (c == '\n') {
		pos_x = 0;
		pos_y++;
	} else if (c >= ' ') {
		*((short *)TEXT + 80*pos_y + pos_x) = MAKEC(c);
		pos_x++;
	}

	if (pos_x >= 80) {
		pos_x = 0;
		pos_y++;
	}
	if (pos_y == 25) {
		pos_y = 24;
		scroll_one_line();
	}
	move_cursor();
}

void __tinit write_s(char *str)
{
	char c;
	while ((c = *(str++)))
		write_c(c);
}

void __tinit sput_c(char **buf, char c)
{
	*((*buf)++) = c;
	**buf = '\0';
}

void __tinit sput_s(char **buf, char *str)
{
	char c;
	while ((c = *(str++)))
		sput_c(buf, c);
}

void __tinit sput_x(char **buf, unsigned int n)
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

void __tinit sput_u(char **buf, unsigned int n)
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

void __tinit sput_d(char **buf, int n)
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
void __tinit vsprint(char *__buf, const char *fmt, va_list ap)
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

char print_buf[1024] = {0};

void __tinit early_print(const char *fmt, ...)
{
	unsigned long flags;
	save_flags(flags);
	cli();

	va_list ap;
	va_start(ap, fmt);
	vsprint(print_buf, fmt, ap);
	va_end(ap);

	write_s(print_buf);
	restore_flags(flags);
}

#include <multiboot.h>

void __tinit early_print_init(struct multiboot_info *m)
{
	unsigned char *s = (unsigned char *)m->boot_loader_name;

	if ((MB_FLAG_LOADER & m->flags) && (0x3f00 == *(unsigned short *)s)) {
		set_pos(s[2], s[3]);
		early_print("%s\n", s + 4);
	} else {
		cls();
	}
}

/*
 * will be removed when the TTY driver is implemented
 */
void sys_putchar(char c)
{
	irq_save();
	write_c(c);
	irq_restore();
}
