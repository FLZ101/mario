#ifndef _SEMAPHORE_H
#define _SEMAPHORE_H

#include <lib/atomic.h>
#include <lib/spinlock.h>

#include <wait.h>

struct semaphore {
	int count;
	spinlock_t lock;
	wait_queue_t wait;
};

#define INIT_MUTEX(sem)	{\
	1, \
	SPINLOCK_UNLOCKED, \
	{ LIST_HEAD_INIT(sem.wait.task_list) }\
}

static inline void sema_init(struct semaphore *sem, int val)
{
	sem->count = val;
	INIT_LOCK(&sem->lock);
	init_wait_queue(&sem->wait);
}

static inline void init_MUTEX(struct semaphore *sem)
{
	sema_init(sem, 1);
}

static inline void init_MUTEX_LOCKED(struct semaphore *sem)
{
	sema_init(sem, 0);
}

void down(struct semaphore *sem);
void up(struct semaphore *sem);

#endif /* _SEMAPHORE_H */