#include <lib/string.h>
#include <lib/stddef.h>

int memcmp(const void *ptr1, const void *ptr2, size_t num)
{
	int n;
	__asm__ (
		"repe cmpsb\n\t"
		"xorl %0, %0\n\t"
		"movb -1(%1), %%al\n\t"
		"subb -1(%2), %%al"
		:"=a"(n)
		:"D"(ptr1), "S"(ptr2), "c"(num)
		:);
	return n;
}

void *memchr(const void *ptr, int c, size_t num)
{
	void *p;
	__asm__ (
		"repne scasb\n\t"
		"je 1f\n\t"
		"xorl %0, %0\n\t"
		"jmp 2f\n"
		"1:\n\t"
		"decl %0\n"
		"2:"
		:"=D"(p)
		:"D"(ptr), "a"(c), "c"(num)
		:);
	return p;
}

/*
 * If coping takes place between overlapping areas,
 * use the memmove function.
 */
void *memcpy(void *dst, const void *src, size_t num)
{
	__asm__ (
		"rep movsb"
		:
		:"D"(dst), "S"(src), "c"(num)
		:"memory");
	return dst;
}

/*
 * It seems that gcc always assumes that DF is cleared when it
 * assembles C source code. Keep this in mind :)
 */
void *memmove(void *dst, const void *src, size_t num)
{
	__asm__ (
		"cmpl %0, %1\n\t"
		"ja 1f\n\t"
		"addl %2, %0\n\t"
		"decl %0\n\t"
		"addl %2, %1\n\t"
		"decl %1\n\t"
		"std\n\t"
		"jmp 2f\n"
		"1:\n\t"
		"cld\n"
		"2:\n\t"
		"rep movsb\n\t"
		"cld"	/* !!! */
		:
		:"D"(dst), "S"(src), "c"(num)
		:"memory");
	return dst;
}

void *memset(void *dst, int c, size_t num)
{
	__asm__ (
		"rep stosb"
		:
		:"D"(dst), "a"(c), "c"(num)
		:"memory");
	return dst;
}

void *memsetw(void *dst, int c, size_t num)
{
	__asm__ (
		"rep stosw"
		:
		:"D"(dst), "a"(c), "c"(num)
		:"memory");
	return dst;
}

void *memsetl(void *dst, int c, size_t num)
{
	__asm__ (
		"rep stosl"
		:
		:"D"(dst), "a"(c), "c"(num)
		:"memory");
	return dst;
}

int strcmp(const char *string1, const char *string2)
{
	int n;
	__asm__ (
		"xorl %0, %0\n"
		"1:\n\t"
		"cmpb $0, (%1)\n\t"
		"je 2f\n\t"
		"cmpsb\n\t"
		"je 1b\n\t"
		"decl %1\n\t"
		"decl %2\n"
		"2:\n\t"
		"movb (%1), %%al\n\t"
		"subb (%2), %%al\n\t"
		"cbtw\n\t"
		"cwtl"
		:"=a"(n)
		:"D"(string1), "S"(string2)
		:);
	return n;
}

int strncmp(const char *string1, const char *string2, size_t num)
{
	int n;
	__asm__ (
		"xorl %0, %0\n"
		"1:\n\t"
		"cmpb $0, (%1)\n\t"
		"je 2f\n\t"
		"cmpsb\n\t"
		"loope 1b\n\t"
		"decl %1\n\t"
		"decl %2\n"
		"2:\n\t"
		"movb (%1), %%al\n\t"
		"subb (%2), %%al\n\t"
		"cbtw\n\t"
		"cwtl"
		:"=a"(n)
		:"D"(string1), "S"(string2), "c"(num)
		:);
	return n;
}

char *strchr(const char *string, int c)
{
	char *p;
	__asm__ (
		"1:\n\t"
		"cmpb $0, (%1)\n\t"
		"je 2f\n\t"
		"scasb\n\t"
		"jne 1b\n\t"
		"decl %1\n\t"
		"jmp 3f\n"
		"2:\n\t"
		"xorl %1, %1\n"
		"3:"
		:"=D"(p)
		:"D"(string), "a"(c)
		:);
	return p;
}

char *strrchr(const char *string, int c)
{
	char *p;
	__asm__ (
		"xorl %0, %0\n"
		"1:\n\t"
		"cmpb $0, (%1)\n\t"
		"je 2f\n\t"
		"scasb\n\t"
		"jne 1b\n\t"
		"movl %1, %0\n\t"
		"decl %0\n\t"
		"jmp 1b\n"
		"2:"
		:"=S"(p)
		:"D"(string), "a"(c)
		:);
	return p;
}

/*
 * If string2 is a NULL string, string1 is returned
 */
char *strstr(const char *string1, const char *string2)
{
	char *p;
	__asm__ (
		"movl %1, %%ebx\n\t"
		"movl %2, %%ecx\n"
		"1:\n\t"
		"movl %%ebx, %1\n\t"
		"cmpb $0, (%1)\n\t"
		"je 2f\n\t"
		"movl %%ecx, %2\n"
		"3:\n\t"
		"lodsb\n\t"
		"testb %%al, %%al\n\t"
		"je 4f\n\t"
		"scasb\n\t"
		"je 3b\n\t"
		"incl %%ebx\n\t"
		"jmp 1b\n"
		"2:\n\t"
		"xorl %%ebx, %%ebx\n"
		"4:"
		:"=b"(p)
		:"D"(string1), "S"(string2)
		:"eax", "ecx");
	return p;
}

char *strcpy(char *dst, const char *src)
{
	__asm__ (
		"1:\n\t"
		"lodsb\n\t"
		"testb %%al, %%al\n\t"
		"je 2f\n\t"
		"stosb\n\t"
		"jmp 1b\n"
		"2:"
		:
		:"D"(dst), "S"(src)
		:"eax", "memory");
	return dst;
}

char *strncpy(char *dst, const char *src, size_t num)
{
	__asm__ (
		"1:\n\t"
		"lodsb\n\t"
		"testb %%al, %%al\n\t"
		"je 2f\n\t"
		"stosb\n\t"
		"loop 1b\n"
		"2:"
		:
		:"D"(dst), "S"(src), "c"(num)
		:"eax", "memory");
	return dst;
}

char *strcat(char *dst, const char *src)
{
	__asm__ (
		"xorb %%al, %%al\n"
		"1:\n\t"
		"scasb\n\t"
		"jne 1b\n\t"
		"decl %0\n"
		"1:\n\t"
		"lodsb\n\t"
		"testb %%al, %%al\n\t"
		"je 2f\n\t"
		"stosb\n\t"
		"jmp 1b\n"
		"2:\n\t"
		"movb $0, (%0)"
		:
		:"D"(dst), "S"(src)
		:"eax", "memory");
	return dst;
}

char *strncat(char *dst, const char *src, size_t num)
{
	__asm__ (
		"xorb %%al, %%al\n"
		"1:\n\t"
		"scasb\n\t"
		"jne 1b\n\t"
		"decl %0\n"
		"1:\n\t"
		"lodsb\n\t"
		"testb %%al, %%al\n\t"
		"je 2f\n\t"
		"stosb\n\t"
		"loop 1b\n"
		"2:\n\t"
		"movb $0, (%0)"
		:
		:"D"(dst), "S"(src), "c"(num)
		:"eax", "memory");
	return dst;
}

char *strpbrk(const char *string1, const char *string2)
{
	return NULL;
}

char *strtok(char *string1, const char *string2)
{
	return NULL;
}
size_t strlen(const char *string)
{
	size_t len;
	__asm__ (
		"repne scasb\n\t"
		"notl %0\n\t"
		"decl %0"
		:"=c"(len)
		:"D"(string), "a"(0), "0"(0xffffffff)
		:);
	return len;
}

size_t strspn(const char *string1, const char *string2)
{
	return 0;
}
size_t strcspn(const char *string1, const char *string2)
{
	return 0;
}