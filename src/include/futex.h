#ifndef _FUTEX_H
#define _FUTEX_H

/*
 * Minimal classic futex operations used by musl on i386.  The private flag
 * is accepted and ignored: Mario does not yet implement private-vs-shared
 * address separation for futexes.
 */
#define FUTEX_WAIT		0
#define FUTEX_WAKE		1
#define FUTEX_FD		2
#define FUTEX_REQUEUE		3
#define FUTEX_CMP_REQUEUE	4
#define FUTEX_WAKE_OP		5
#define FUTEX_LOCK_PI		6
#define FUTEX_UNLOCK_PI		7
#define FUTEX_TRYLOCK_PI	8
#define FUTEX_WAIT_BITSET	9
#define FUTEX_WAKE_BITSET	10

#define FUTEX_PRIVATE		128
#define FUTEX_CLOCK_REALTIME	256

void futex_init(void);
int futex_wake(int *uaddr, int nr_wake);

#endif /* _FUTEX_H */
