#ifndef _SPINLOCK_H
#define _SPINLOCK_H

/*
 * A spinlock protects data that may be touched by more than one CPU.  On
 * current single-CPU Mario builds the existing callers also disable
 * interrupts, but once APs are online only a locked RMW is sufficient.
 * Plain `xchg` is architecturally atomic on x86, yet it does not by itself
 * express the acquire/release ordering GCC expects around the lock; the
 * memory clobbers below make that ordering explicit.
 */
typedef struct {
	volatile unsigned int locked;
} spinlock_t;

#define SPINLOCK_UNLOCKED (spinlock_t){0}

#define INIT_LOCK(lock) \
do { \
	(lock)->locked = 0; \
} while (0)

static __inline__ void spin_lock(spinlock_t *lock)
{
	__asm__ __volatile__(
		"1:\n\t"
		/* `lock btsl` is an atomic test-and-set on the cache line. */
		"lock btsl $0, %0\n\t"
		"jnc 3f\n\t"
		"2:\n\t"
		/* Spin on a plain load to avoid hammering the bus. */
		"pause\n\t"
		"cmpl $0, %0\n\t"
		"jne 2b\n\t"
		"jmp 1b\n\t"
		"3:"
		: "+m"(lock->locked)
		:
		: "memory");
}

static __inline__ void spin_unlock(spinlock_t *lock)
{
	/*
	 * `xchgl` with a zero is an atomic release: writes made while holding
	 * the lock are globally visible before the lock word becomes free.
	 */
	unsigned int zero = 0;
	__asm__ __volatile__(
		"xchgl %0, %1"
		: "=r"(zero), "+m"(lock->locked)
		: "0"(zero)
		: "memory");
}

/* Legacy names kept for existing kernel call sites. */
#define ACQUIRE_LOCK(lock) spin_lock(lock)
#define RELEASE_LOCK(lock) spin_unlock(lock)

/*
 * Interrupt-safe variants.  They are safe on one CPU and become the standard
 * pattern on SMP: disable local interrupts, then take the lock.
 */
#define spin_lock_irqsave(lock, flags) \
do { \
	__asm__ __volatile__("pushfl; popl %0; cli" \
			     : "=g"(flags) \
			     : \
			     : "memory"); \
	spin_lock(lock); \
} while (0)

#define spin_unlock_irqrestore(lock, flags) \
do { \
	spin_unlock(lock); \
	__asm__ __volatile__("pushl %0; popfl" \
			     : \
			     : "g"(flags) \
			     : "memory"); \
} while (0)

#endif	/* _SPINLOCK_H */
