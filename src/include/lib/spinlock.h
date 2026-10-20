#ifndef _SPINLOCK_H
#define _SPINLOCK_H

typedef struct {
	unsigned int locked;
} spinlock_t;

#define SPINLOCK_UNLOCKED (spinlock_t){0}

#define INIT_LOCK(lock) \
do { \
	(lock)->locked = 0; \
} while (0)

#define ACQUIRE_LOCK(lock) \
do { \
	unsigned int __lock_value = 1; \
	__asm__ __volatile__( \
		"1:\n" \
		"xchgl %0, %1\n\t" \
		"andl %1, %1\n\t" \
		"jnz 1b" \
		:"+m"((lock)->locked), "+a"(__lock_value) \
		: \
		:"cc"); \
} while (0)

#define RELEASE_LOCK(lock) \
do { \
	unsigned int __lock_value = 0; \
	__asm__ __volatile__ ( \
		"xchgl %0, %1" \
		:"+m"((lock)->locked), "+a"(__lock_value) \
		: ); \
} while(0)

#endif	/* _SPINLOCK_H */
