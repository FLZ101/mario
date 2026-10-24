#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	fprintf(stderr, "unknown pthread stress mode: %s\n", mode);
	return 2;
}
