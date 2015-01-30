#ifndef _TEXT_H
#define _TEXT_H

unsigned char inportb(unsigned short _port);
void outportb(unsigned short _port, unsigned char _data);
void init_text(void);
void set_color(int C);
void set_cursor(int x, int y);
void move_cursor(void);
void clear(void);
void scroll_one_line(void);
void put_c(unsigned char c);
void put_s(char *str);
void put_x(unsigned int n);
void put_u(unsigned int n);
void put_d(int n);
void printf(const char *fmt, ...);
#endif	/* _TEXT_H */