#include <stdarg.h>
#include <string.h>
#include <misc.h>
#include <io.h>

#define COLOR 0x07

#define MAKEC(c) (COLOR << 8 | (c))

#define SPACE MAKEC(' ')

int __dinit pos_x = 0;
int __dinit pos_y = 0;

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
	memsetw((void *)0xb8000, SPACE, 80*25);
	set_pos(0, 0);
}

void __tinit scroll_one_line(void)
{
	memmove((void *)0xb8000, (void *)(0xb8000 + 80*2), 80*2*24);
	memsetw((void *)(0xb8000 + 80*2*24), SPACE, 80);
}

void __tinit put_c(unsigned char c)
{
	if (c == '\t') {
		pos_x += 8;
		pos_x &= ~7;
	} else if (c == '\n') {
		pos_x = 0;
		pos_y++;
	} else if (c >= ' ') {
		*((short *)0xb8000 + 80*pos_y + pos_x) = MAKEC(c);
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

void __tinit put_s(char *str)
{
	char c;
	while ((c = *(str++)))
		put_c(c);
}

void __tinit put_x(unsigned int n)
{
	int i, j;
	char s[11] = {'0', 'x'};

	for (i = 9; i > 1; i--) {
		if (n) {
			j = n;
			n = n >> 4;
			j = j - (n << 4);
			if (j < 10)
				s[i] = j + '0';
			else
				s[i] = j - 10 + 'a';
		} else {
			s[i] = '0';
		}
	}
	put_s(s);
}

void __tinit put_u(unsigned int n)
{
	int i, j;
	char s[9] = {0};
	
	if (n == 0) {
		put_c('0');
		return;
	}
	
	for (i = 7; i >= 0; i--) {
		if (n) {
			j = n;
			n = n/10;
			s[i] = j - n*10 + '0';
		} else {
			break;
		}
	}
	put_s(s + i + 1);
}

/*
 * %u, %x, %c, %s
 */
void __tinit early_print(const char *fmt, ...)
{	
	va_list ap;
	va_start(ap, fmt);
	
	char c;
	while ((c = *(fmt++))) {
		if (c == '%') {
			c = *(fmt++);
			switch (c) {
			case 'u':
				put_u(va_arg(ap, unsigned int));
				break;
			case 'x':
				put_x(va_arg(ap, unsigned int));
				break;
			case 'c':
				put_c(va_arg(ap, char));
				break;
			case 's':
				put_s(va_arg(ap, char *));
				break;
			default:
				break;
			}
			continue;
		}
		put_c(c);
	}
	va_end(ap);		
}

#include <multiboot.h>

void __tinit early_print_init(struct multiboot_info *m)
{
	unsigned char *s = (unsigned char *)m->boot_loader_name;

	if (MB_FLAG_LOADER & m->flags && (0x3f00 == *(unsigned short *)s)) {
		set_pos(s[2], s[3]);
		early_print("%s\n", s + 4);
	} else {
		cls();
	}
}