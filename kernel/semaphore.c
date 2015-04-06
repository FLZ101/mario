#include <semaphore.h>
#include <sched.h>

void down(struct semaphore *sem)
{
	ACQUIRE_LOCK(&sem->lock);
	if (--sem->count < 0)
		sleep_on(&sem->wait, TASK_UNINTERRUPTIBLE, &sem->lock);
	else
		RELEASE_LOCK(&sem->lock);
}

void up(struct semaphore *sem)
{
	ACQUIRE_LOCK(&sem->lock);
	if (++sem->count <= 0)
		wake_up_1st(&sem->wait, TASK_UNINTERRUPTIBLE);
	RELEASE_LOCK(&sem->lock);
}