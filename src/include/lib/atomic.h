#ifndef _ATOMIC_H
#define _ATOMIC_H

typedef struct { volatile int counter; } atomic_t;

#define ATOMIC_INIT(i)	{ (i) }

#define atomic_read(v)		((v)->counter)
#define atomic_set(v,i)		(((v)->counter) = (i))

static __inline__ void atomic_add(int i, atomic_t *v)
{
	__asm__ __volatile__(
		"addl %1, %0"
		:"=m"(v->counter)
		:"ir"(i), "m"(v->counter));
}

static __inline__ void atomic_sub(int i, atomic_t *v)
{
	__asm__ __volatile__(
		"subl %1, %0"
		:"=m"(v->counter)
		:"ir"(i), "m"(v->counter));
}

static __inline__ int atomic_sub_and_test(int i, atomic_t *v)
{
	unsigned char c;

	__asm__ __volatile__(
		"subl %2, %0\n\t"
		"sete %1"
		:"=m"(v->counter), "=qm"(c)
		:"ir"(i), "m"(v->counter)
		:"memory");
	return c;
}

static __inline__ void atomic_inc(atomic_t *v)
{
	__asm__ __volatile__(
		"incl %0"
		:"=m"(v->counter)
		:"m"(v->counter));
}

static __inline__ void atomic_dec(atomic_t *v)
{
	__asm__ __volatile__(
		"decl %0"
		:"=m"(v->counter));
}

static __inline__ int atomic_dec_and_test(atomic_t *v)
{
	unsigned char c;

	__asm__ __volatile__(
		"decl %0\n\t"
		"sete %1"
		:"=m"(v->counter), "=qm"(c)
		:"m"(v->counter)
		:"memory");
	return c != 0;
}

static __inline__ int atomic_inc_and_test(atomic_t *v)
{
	unsigned char c;

	__asm__ __volatile__(
		"incl %0\n\t"
		"sete %1"
		:"=m"(v->counter), "=qm"(c)
		:"m"(v->counter)
		:"memory");
	return c != 0;
}

static __inline__ int atomic_add_negative(int i, atomic_t *v)
{
	unsigned char c;

	__asm__ __volatile__(
		"addl %2, %0\n\t"
		"sets %1"
		:"=m"(v->counter), "=qm"(c)
		:"ir"(i), "m"(v->counter)
		:"memory");
	return c;
}

#endif	/* _ATOMIC_H */