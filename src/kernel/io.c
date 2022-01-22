#include <io.h>

unsigned char inb(unsigned short port)
{
	unsigned char ret;
	__asm__ __volatile__ ("inb %%dx, %%al":"=a"(ret):"d"(port));
	return ret;
}

unsigned short inw(unsigned short port)
{
	unsigned short ret;
	__asm__ __volatile__ ("inw %%dx, %%ax":"=a"(ret):"d"(port));
	return ret;
}

unsigned int inl(unsigned short port)
{
	unsigned int ret;
	__asm__ __volatile__ ("inl %%dx, %%eax":"=a"(ret):"d"(port));
	return ret;
}

void outb(unsigned short port, unsigned char data)
{
	__asm__ __volatile__ ("outb %%al, %%dx": :"d"(port), "a"(data));
}

void outw(unsigned short port, unsigned short data)
{
	__asm__ __volatile__ ("outw %%ax, %%dx": :"d"(port), "a"(data));
}

void outl(unsigned short port, unsigned int data)
{
	__asm__ __volatile__ ("outl %%eax, %%dx": :"d"(port), "a"(data));
}