#include <lib/bitops.h>

/*
 * Search a bitmap at <addr> that contains <size> bits for the first 
 * zero bit, and return the offset of that bit. If the bitmap contains 
 * no zero bit, -1 is returned.
 */
int find_first_zero_bit(void *addr, unsigned int size)
{
	int res;

	__asm__ __volatile__(
		"movl $-1, %%eax\n\t"
		"repe scasl\n\t"
		"jne 2f\n"
		"1:\n\t"
		"movl $-1, %%edx\n\t"
		"jmp 3f\n"
		"2:\n\t"
		"subl $4, %%edi\n\t"
		"xorl (%%edi), %%eax\n\t"
		"bsfl %%eax, %%edx\n\t"
		"subl %%ebx, %%edi\n\t"
		"shll $3, %%edi\n\t"
		"addl %%edi, %%edx\n\t"
		"cmpl %%esi, %%edx\n\t"
		"jae 1b\n"
		"3:"
		:"=d"(res)
		:"c"((size + 31) >> 5), "D"(addr), "b"(addr), "S"(size)
		:"eax");
	return res;
}

/*
 * Search a bitmap at <addr> that contains <size> bits for the next zero 
 * bit starting from <offset + 1>, and return the offset of that bit. If 
 * the zero bit is not found, -1 is returned.
 */
int find_next_zero_bit(void *addr, unsigned int size, unsigned int offset)
{
	if (size <= ++offset)
		return -1;

	unsigned long *p = ((unsigned long *)addr) + (offset >> 5);
	int set = 0, bit = offset & 31, res;

	if (bit) {
		__asm__ __volatile__(
			"bsfl %1, %0\n\t"
			"jne 1f\n\t"
			"movl $32, %0\n"
			"1:"
			:"=r"(set)
			:"r"(~(*p >> bit))
			:);
		if (set + bit < 32) {
			offset += set;
			return offset < size ? offset : -1;
		}
		set = 32 - bit;
		p++;
	}

	res = find_first_zero_bit(p, size - 32*(p - (unsigned long *)addr));
	return (-1 == res) ? -1 : offset + set + res;
}