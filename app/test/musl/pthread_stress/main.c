#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Multi-mode musl pthread stress tests.  Each mode is deliberately small and
 * prints a stable ok tag so shell lit tests can exercise different thread
 * lifetimes without timing out in QEMU.
 */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int counter;

static int arg_int(int argc, char **argv, int idx, int fallback)
{
	int value;

	if (idx >= argc)
		return fallback;
	value = atoi(argv[idx]);
	return value > 0 ? value : fallback;
}

static void *counter_worker(void *arg)
{
	int rounds = (int)(long)arg;
	int i;

	for (i = 0; i < rounds; i++) {
		pthread_mutex_lock(&lock);
		counter++;
		pthread_mutex_unlock(&lock);
	}
	return 0;
}

static int run_create_join(int argc, char **argv)
{
	int rounds = arg_int(argc, argv, 2, 5);
	int threads = arg_int(argc, argv, 3, 8);
	int iters = arg_int(argc, argv, 4, 5000);
	pthread_t tids[64];
	int r, t;

	if (threads > 64)
		threads = 64;
	for (r = 0; r < rounds; r++) {
		counter = 0;
		for (t = 0; t < threads; t++) {
			if (pthread_create(&tids[t], 0, counter_worker,
					   (void *)(long)iters))
				return 1;
		}
		for (t = 0; t < threads; t++)
			if (pthread_join(tids[t], 0))
				return 1;
		if (counter != threads * iters)
			return 1;
	}
	puts("create-join-ok");
	return 0;
}

static void *detached_worker(void *arg)
{
	int rounds = (int)(long)arg;
	int i;

	for (i = 0; i < rounds; i++) {
		pthread_mutex_lock(&lock);
		counter++;
		pthread_mutex_unlock(&lock);
	}
	return 0;
}

static int run_detached(int argc, char **argv)
{
	int threads = arg_int(argc, argv, 2, 8);
	int iters = arg_int(argc, argv, 3, 5000);
	pthread_t tid;
	pthread_attr_t attr;
	int i;

	counter = 0;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (i = 0; i < threads; i++) {
		if (pthread_create(&tid, &attr, detached_worker,
				   (void *)(long)iters))
			return 1;
	}
	pthread_attr_destroy(&attr);

	while (1) {
		int done;

		pthread_mutex_lock(&lock);
		done = counter == threads * iters;
		pthread_mutex_unlock(&lock);
		if (done)
			break;
		usleep(1000);
	}
	puts("detached-ok");
	return 0;
}

struct nested_args {
	int children;
	int iters;
	int target;
};

static int nested_done;

static void *nested_leaf(void *arg)
{
	int iters = (int)(long)arg;
	int i;

	for (i = 0; i < iters; i++) {
		pthread_mutex_lock(&lock);
		counter++;
		pthread_mutex_unlock(&lock);
	}
	pthread_mutex_lock(&lock);
	nested_done++;
	pthread_mutex_unlock(&lock);
	return 0;
}

static void *nested_worker(void *arg)
{
	struct nested_args *args = arg;
	pthread_t tids[16];
	int i;

	if (args->children > 16)
		args->children = 16;
	for (i = 0; i < args->children; i++) {
		if (pthread_create(&tids[i], 0, nested_leaf,
				   (void *)(long)args->iters))
			return (void *)1;
	}

	while (1) {
		int done;

		pthread_mutex_lock(&lock);
		done = nested_done == args->target;
		pthread_mutex_unlock(&lock);
		if (done)
			break;
		usleep(1000);
	}
	return 0;
}

static int run_nested(int argc, char **argv)
{
	int parents = arg_int(argc, argv, 2, 4);
	int children = arg_int(argc, argv, 3, 4);
	int iters = arg_int(argc, argv, 4, 5000);
	struct nested_args args;
	pthread_t tids[32];
	int i;

	if (parents > 32)
		parents = 32;
	args.children = children;
	args.iters = iters;
	args.target = parents * children;
	nested_done = 0;
	for (i = 0; i < parents; i++) {
		if (pthread_create(&tids[i], 0, nested_worker, &args))
			return 1;
	}
	for (i = 0; i < parents; i++) {
		void *ret = (void *)1;

		if (pthread_join(tids[i], &ret) || ret != 0)
			return 1;
	}
	puts("nested-ok");
	return 0;
}

static int run_many(int argc, char **argv)
{
	int threads = arg_int(argc, argv, 2, 32);
	int iters = arg_int(argc, argv, 3, 2000);
	pthread_t tids[64];
	int i;

	if (threads > 64)
		threads = 64;
	counter = 0;
	for (i = 0; i < threads; i++)
		if (pthread_create(&tids[i], 0, counter_worker,
				   (void *)(long)iters))
			return 1;
	for (i = 0; i < threads; i++)
		if (pthread_join(tids[i], 0))
			return 1;
	if (counter != threads * iters)
		return 1;
	puts("many-ok");
	return 0;
}

/*
 * Keep a pool of independent workers busy for a short wall-clock interval.
 * Unlike the counter modes above, each thread mostly touches only its own
 * progress slot, so the test exercises genuinely concurrent pthread activity
 * rather than a mutex serializing almost all useful work.
 */
static pthread_mutex_t parallel_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int parallel_ready;
static volatile int parallel_start;
static volatile int parallel_stop;
static unsigned long parallel_work[64];
static unsigned long parallel_rounds[64];

/*
 * Keep each worker in userspace for a meaningful number of iterations before
 * calling into the kernel.  usleep(0) is used as a cheap voluntary yield: it
 * still goes through nanosleep/schedule_timeout(0), so AP-only runqueues can
 * make progress without adding an actual sleep to the measured workload.
 */
#define PARALLEL_COMPUTE_ROUNDS 200000

static void *parallel_worker(void *arg)
{
	int id = (int)(long)arg;
	unsigned long local = 0;
	int i;

	pthread_mutex_lock(&parallel_lock);
	parallel_ready++;
	pthread_mutex_unlock(&parallel_lock);

	while (!parallel_start)
		usleep(100);

	while (!parallel_stop) {
		for (i = 0; i < PARALLEL_COMPUTE_ROUNDS; i++)
			local = local * 33 + (unsigned long)i;
		parallel_work[id] = local;
		parallel_rounds[id]++;
		usleep(0);
	}
	return 0;
}

static int run_parallel(int argc, char **argv)
{
	int threads = arg_int(argc, argv, 2, 8);
	int duration_ms = arg_int(argc, argv, 3, 2000);
	long duration_us = (long)duration_ms * 1000;
	pthread_t tids[64];
	int i;

	if (threads > 64)
		threads = 64;
	if (duration_us > 10000000)
		duration_us = 10000000;

	parallel_ready = 0;
	parallel_start = 0;
	parallel_stop = 0;
	memset(parallel_work, 0, sizeof(parallel_work));
	memset(parallel_rounds, 0, sizeof(parallel_rounds));

	for (i = 0; i < threads; i++) {
		if (pthread_create(&tids[i], 0, parallel_worker,
				   (void *)(long)i))
			return 1;
	}

	while (1) {
		int ready;

		pthread_mutex_lock(&parallel_lock);
		ready = parallel_ready;
		pthread_mutex_unlock(&parallel_lock);
		if (ready == threads)
			break;
		usleep(1000);
	}

	parallel_start = 1;
	usleep((useconds_t)duration_us);
	parallel_stop = 1;

	for (i = 0; i < threads; i++)
		if (pthread_join(tids[i], 0))
			return 1;

	for (i = 0; i < threads; i++)
		if (parallel_rounds[i] == 0)
			return 1;

	puts("parallel-ok");
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "create-join";

	if (!strcmp(mode, "create-join"))
		return run_create_join(argc, argv);
	if (!strcmp(mode, "detached"))
		return run_detached(argc, argv);
	if (!strcmp(mode, "nested"))
		return run_nested(argc, argv);
	if (!strcmp(mode, "many"))
		return run_many(argc, argv);
	if (!strcmp(mode, "parallel"))
		return run_parallel(argc, argv);
	fprintf(stderr, "unknown pthread stress mode: %s\n", mode);
	return 2;
}
