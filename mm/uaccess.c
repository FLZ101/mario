#include <mm/uaccess.h>

int verify_area(int type, const void *addr, unsigned long size)
{
	return 0;
}

void memcpy_fromfs(void *to, void *from, unsigned int n)
{
	__asm__ __volatile__ (
		"1:\n\t"
		"movb %%fs:(%1), %%al\n\t"
		"movb %%al, (%0)\n\t"
		"incl %0\n\t"
		"incl %1\n\t"
		"loop 1b"
		:
		:"D"(to), "S"(from), "c"(n)
		:"eax", "memory");
}

void memcpy_tofs(void *to, void *from, unsigned int n)
{
	__asm__ __volatile__ (
		"1:\n\t"
		"movb (%1), %%al\n\t"
		"movb %%al, %%fs:(%0)\n\t"
		"incl %0\n\t"
		"incl %1\n\t"
		"loop 1b"
		:
		:"D"(to), "S"(from), "c"(n)
		:"eax", "memory");
}
