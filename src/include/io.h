#ifndef _IO_H
#define _IO_H

unsigned char inb(unsigned short port);
unsigned short inw(unsigned short port);
unsigned int inl(unsigned short port);

void outb(unsigned short port, unsigned char data);
void outw(unsigned short port, unsigned short data);
void outl(unsigned short port, unsigned int data);

#endif	/* _IO_H */
