#ifndef _TIME_H
#define _TIME_H

#define HZ 100

void time_init(void);

struct timespec {
	long	tv_sec;		/* seconds */
	long	tv_nsec;	/* nanoseconds */
};

extern volatile long jiffies;

struct timeval {
	long tv_sec;
	long tv_usec;
};

struct itimerval {
	struct	timeval it_interval;
	struct	timeval it_value;
};

#define	ITIMER_REAL	0
#define	ITIMER_VIRTUAL	1
#define	ITIMER_PROF	2

typedef struct fd_set {
	unsigned long fds_bits[8];
} fd_set;

#define NFDBITS	(8 * sizeof(unsigned long))

#define FD_SETSIZE	(8 * NFDBITS)

#define FD_SET(fd, fdsetp) \
__asm__ __volatile__( \
	"btsl %1, %0" \
	:"=m" (*(fd_set *) (fdsetp)) \
	:"r" ((int) (fd)) \
	:)

#define FD_CLR(fd, fdsetp) \
__asm__ __volatile__( \
	"btrl %1, %0" \
	:"=m" (*(fd_set *) (fdsetp)) \
	:"r" ((int) (fd)) \
	:)

#define FD_ISSET(fd,fdsetp) (__extension__ \
({ \
	unsigned char __result; \
	__asm__ __volatile__( \
		"btl %1, %2; setb %0" \
		:"=q" (__result) \
		:"r" ((int) (fd)), "m" (*(fd_set *) (fdsetp)) \
		:); \
	__result; \
}))

#define FD_ZERO(fdsetp) \
__asm__ __volatile__( \
	"cld; rep; stosl" \
	:"=m" (*(fd_set *) (fdsetp)) \
	:"a" (0), "c" (8), "D" ((fd_set *) (fdsetp)) \
	:)

#endif	/* _TIME_H */