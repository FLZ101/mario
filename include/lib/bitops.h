#ifndef _BITOPS_H
#define _BITOPS_H

#define ADDR (*(volatile long *)addr)

static __inline__ void set_bit(int nr, volatile void *addr)
{
	__asm__ __volatile__(
		"btsl %1, %0"
		:"=m"(ADDR)
		:"Ir"(nr));
}

static __inline__ void clear_bit(int nr, volatile void *addr)
{
	__asm__ __volatile__ (
		"btrl %1, %0"
		:"=m"(ADDR)
		:"Ir"(nr));
}

static __inline__ void change_bit(int nr, volatile void *addr)
{
	__asm__ __volatile__(
		"btcl %1, %0"
		:"=m"(ADDR)
		:"Ir"(nr));
}

static __inline__ int test_bit(int nr, volatile void *addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btl %2, %1\n\t"
		"sbbl %0, %0"
		:"=r"(oldbit), "=m"(ADDR)
		:"Ir"(nr)
		:"memory");
	return oldbit;
}

static __inline__ int test_and_set_bit(int nr, volatile void *addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btsl %2, %1\n\t"
		"sbbl %0, %0"
		:"=r"(oldbit), "=m"(ADDR)
		:"Ir"(nr)
		:"memory");
	return oldbit;
}

static __inline__ int test_and_clear_bit(int nr, volatile void *addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btrl %2, %1\n\t"
		"sbbl %0, %0"
		:"=r"(oldbit), "=m"(ADDR)
		:"Ir"(nr)
		:"memory");
	return oldbit;
}

static __inline__ int test_and_change_bit(int nr, volatile void *addr)
{
	int oldbit;

	__asm__ __volatile__(
		"btcl %2, %1\n\t"
		"sbbl %0, %0"
		:"=r"(oldbit), "=m"(ADDR)
		:"Ir"(nr)
		:"memory");
	return oldbit;
}

/*
 * Find first zero bit in a word, return the offset of that bit. If 
 * the word contains no zero bit, -1 is returned.
 */
static __inline__ int ffz(unsigned long word)
{
	__asm__(
		"bsfl %1, %0\n\t"
		"jne 1f\n\t"
		"movl $-1, %0\n"
		"1:"
		:"=r"(word)
		:"r"(~word));
	return word;
}

/*
 * Find first bit set in a word, return the offset of that bit. If 
 * the word contains no bit set, -1 is returned.
 */
static __inline__ int ffs(unsigned long word)
{
	return ffz(~word);
}

int find_first_zero_bit(void *addr, unsigned int size);

int find_next_zero_bit(void *addr, unsigned int size, unsigned int offset);

#endif	/* _BITOPS_H */