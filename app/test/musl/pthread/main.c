#include <pthread.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

/*
 * Exercise the clone/futex pthread path with a small contended counter.  The
 * shell test runs this with eight CPUs so clone children spread across the
 * per-CPU runqueues.
 */
#define THREADS 8
#define ITERATIONS 20000

static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static pid_t worker_tids[THREADS];

static void *worker(void *arg)
{
	long id = (long)arg;
	int i;

	worker_tids[id] = syscall(SYS_gettid);
	for (i = 0; i < ITERATIONS; i++) {
		pthread_mutex_lock(&counter_lock);
		counter++;
		pthread_mutex_unlock(&counter_lock);
	}
	return (void *)(id + 1);
}

int main(void)
{
	pthread_t threads[THREADS];
	void *ret;
	pid_t main_tid = syscall(SYS_gettid);
	int i;

	printf("main pid=%d tid=%d\n", getpid(), main_tid);
	for (i = 0; i < THREADS; i++) {
		int rc = pthread_create(&threads[i], NULL, worker, (void *)(long)i);
		if (rc) {
			printf("pthread_create %d failed: %d\n", i, rc);
			return 1;
		}
	}

	for (i = 0; i < THREADS; i++) {
		int rc = pthread_join(threads[i], &ret);
		if (rc || ret != (void *)(long)(i + 1)) {
			printf("pthread_join %d failed rc=%d ret=%p\n", i, rc, ret);
			return 1;
		}
	}

	printf("counter=%d\n", counter);
	if (counter != THREADS * ITERATIONS)
		return 1;

	for (i = 0; i < THREADS; i++) {
		int j;
		if (worker_tids[i] == main_tid)
			return 1;
		for (j = i + 1; j < THREADS; j++)
			if (worker_tids[i] == worker_tids[j])
				return 1;
	}
	puts("pthread-ok");
	return 0;
}
