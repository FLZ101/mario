#include <stdarg.h>
#include <string.h>

unsigned char inportb(unsigned short _port)
{
	unsigned char rv;
	__asm__ __volatile__ ("inb %1,%0" : "=a"(rv) : "dN"(_port));
	return rv;
}

void outportb(unsigned short _port, unsigned char _data)
{
	__asm__ __volatile__ ("outb %1,%0" : : "dN"(_port), "a"(_data));
}

int pos_x;
int pos_y;
int color;

void init_text(void)
{
	pos_x = 0;
	pos_y = 0;
	color = 0x07;
}

void set_color(int C) 
{
	color = C;
}

void set_cursor(int x, int y)
{
	pos_x = x;
	pos_y = y;
}

void move_cursor(void)
{
	unsigned tmp = pos_y*80 + pos_x;
	
	outportb(0x3d4, 14);
	outportb(0x3d5, *((char *)(&tmp) + 1));
	outportb(0x3d4, 15);
	outportb(0x3d5, tmp);
}

void clear(void)
{
	memsetw((void *)0xb8000, ' ' | (color << 8), 80*25);
	pos_x = 0;
	pos_y = 0;
	move_cursor();
}

void scroll_one_line(void)
{
	memmove((void *)0xb8000, (void *)(0xb8000 + 80*2), 80*2*24);
	memsetw((void *)(0xb8000 + 80*2*24), ' ' | (color << 8), 80);
}


void put_c(unsigned char c)
{
	if (c == '\b') {
		if (pos_x != 0) 
			pos_x--;
	}
	else if (c == '\t') {
		pos_x += 8;
		pos_x &= ~7;
	}
	else if (c == '\r') {
		pos_x = 0;
	}
	else if (c == '\n') {
		pos_x = 0;
		pos_y++;
	}
	else if(c >= ' ') {
		short *tmp = (short *)0xb8000;
		tmp += (80*pos_y + pos_x);
		*tmp = c | (color << 8);
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

void put_s(char *str)
{
	char c = *str;
	for (; c != '\0'; put_c(c), str++, c = *str);
}

void put_x(unsigned int n)
{
	int i;
	int j;
	char to_print[9];
	
	to_print[8] = 0;
	
	for (i = 7; i > -1; i--) {
		if (n == 0)
			to_print[i] = '0';
		else {
			j = n;
			n = n >> 4;
			j = j - (n << 4);
			if (j < 10)
				to_print[i] = j + '0';
			else
				to_print[i] = j - 10 + 'a';
		}
	}
	put_s("0x");
	put_s(to_print);
}

void put_u(unsigned int n)
{
	int i;
	int j;
	char to_print[11];
	
	to_print[10] = 0;
	
	if (n == 0) {
		put_c('0');
		return ;
	}
	
	for (i = 9; i >= 0; i--) {
		if (n == 0)
			break;
		else {
			j = n;
			n = n / 10;
			to_print[i] = j - n * 10 + 48;
		}
	}
	put_s(to_print + i + 1);
}

void put_d(int n)
{
	if (n < 0) {
		put_c('-');
		n = ~n + 1;
	}
	put_u(n);
}

//%d, %u, %x, %c, %s
void printf(const char *fmt, ...)
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
					continue;
				case 'd':
					put_d(va_arg(ap, int));
					continue;
				case 'x':
					put_x(va_arg(ap, unsigned int));
					continue;
				case 'c':
					put_c(va_arg(ap, char));
					continue;
				case 's':
					put_s(va_arg(ap, char *));
					continue;
			}
		}
		put_c(c);
	}
	va_end(ap);		
}