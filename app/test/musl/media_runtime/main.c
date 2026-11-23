#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REQUIRE(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "media-runtime: line %d: %s (errno=%d)\n", \
			__LINE__, #condition, errno); \
		_exit(1); \
	} \
} while (0)

static const long double parent_marker = 0x1.123456789abcdefp+3L;
static const long double child_marker = -0x1.fedcba987654321p-4L;
static const unsigned short default_control = 0x037f;
static const unsigned short parent_control = 0x077f;
static const unsigned short child_control = 0x0b7f;
static volatile sig_atomic_t handled;

static unsigned short x87_control(void)
{
	unsigned short control;

	asm volatile("fnstcw %0" : "=m"(control));
	return control;
}

static void x87_push(const long double *marker, unsigned short control)
{
	asm volatile("fldcw %0; fldt %1" : : "m"(control), "m"(*marker) : "st");
}

static void x87_check(const long double *marker, unsigned short control)
{
	long double actual;

	REQUIRE(x87_control() == control);
	asm volatile("fstpt %0" : "=m"(actual) : : "st");
	REQUIRE(!memcmp(&actual, marker, 10));
}

static void x87_empty(void)
{
	unsigned short status;

	asm volatile("fnstsw %0" : "=am"(status));
	REQUIRE(x87_control() == default_control);
	REQUIRE(!(status & 0x3800));
}

static void x87_invalid(void)
{
	const unsigned short unmasked = 0x037e;
	asm volatile("fninit; fldcw %0; fldz; fldz; fdivp" : : "m"(unmasked) : "st", "st(1)");
}

static void check_context(void)
{
	int to_child[2], to_parent[2];
	char byte = 'x';
	int status;

	REQUIRE(!pipe(to_child) && !pipe(to_parent));
	x87_push(&parent_marker, parent_control);
	pid_t child = fork();
	REQUIRE(child >= 0);
	if (!child) {
		x87_check(&parent_marker, parent_control);
		for (int cycle = 0; cycle < 16; ++cycle) {
			x87_push(&child_marker, child_control);
			REQUIRE(read(to_child[0], &byte, 1) == 1);
			x87_check(&child_marker, child_control);
			REQUIRE(write(to_parent[1], &byte, 1) == 1);
		}
		_exit(0);
	}
	for (int cycle = 0; cycle < 16; ++cycle) {
		struct pollfd reply = { .fd = to_parent[0], .events = POLLIN };

		REQUIRE(write(to_child[1], &byte, 1) == 1);
		REQUIRE(poll(&reply, 1, 2500) == 1 && (reply.revents & POLLIN));
		REQUIRE(read(to_parent[0], &byte, 1) == 1);
		x87_check(&parent_marker, parent_control);
		x87_push(&parent_marker, parent_control);
	}
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	x87_check(&parent_marker, parent_control);
	x87_push(&parent_marker, parent_control);
	execl("/usr/bin/test/media_runtime.exe", "media_runtime", "exec", NULL);
	REQUIRE(0);
}

static void clobber_x87(int signum)
{
	if (signum == SIGUSR1) {
		x87_push(&child_marker, child_control);
		REQUIRE(!raise(SIGUSR2));
		x87_check(&child_marker, child_control);
	} else {
		asm volatile("fninit");
		x87_push(&parent_marker, parent_control);
		x87_check(&parent_marker, parent_control);
		x87_invalid();
	}
	handled |= signum == SIGUSR1 ? 1 : 2;
}

static void check_signals(void)
{
	struct sigaction action = { .sa_handler = clobber_x87 };
	int status;

	REQUIRE(!sigemptyset(&action.sa_mask));
	REQUIRE(!sigaction(SIGUSR1, &action, NULL));
	REQUIRE(!sigaction(SIGUSR2, &action, NULL));
	x87_push(&parent_marker, parent_control);
	REQUIRE(!raise(SIGUSR1));
	REQUIRE(handled == 3);
	x87_check(&parent_marker, parent_control);
	pid_t child = fork();
	REQUIRE(child >= 0);
	if (!child) {
		x87_invalid();
		asm volatile("fwait");
		_exit(1);
	}
	REQUIRE(waitpid(child, &status, 0) == child);
	REQUIRE(WIFSIGNALED(status) && WTERMSIG(status) == SIGFPE);
	puts("media-runtime: signals ok");
}

static unsigned int apic_id(void)
{
	unsigned int apic;

	unsigned int leaf = 1;
	asm volatile("cpuid" : "+a"(leaf), "=b"(apic) : : "ecx", "edx");
	return apic >> 24;
}

static void pin_to_cpu(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	REQUIRE(!sched_setaffinity(0, sizeof(set), &set));
}

static void check_migration(void)
{
	pin_to_cpu(0);
	unsigned int first_id = apic_id();

	for (int cycle = 0; cycle < 16; ++cycle) {
		x87_push(&parent_marker, parent_control);
		pin_to_cpu(1);
		REQUIRE(apic_id() != first_id);
		x87_check(&parent_marker, parent_control);
		x87_push(&child_marker, child_control);
		pin_to_cpu(0);
		REQUIRE(apic_id() == first_id);
		x87_check(&child_marker, child_control);
	}
	puts("media-runtime: migrated ok");
}

static int64_t monotonic_ms(void)
{
	struct timespec now;

	REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &now));
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

struct cond_fixture {
	pthread_mutex_t lock;
	pthread_cond_t cond;
	int ready;
};

static void *signal_cond(void *arg)
{
	struct cond_fixture *fixture = arg;
	const struct timespec delay = { .tv_nsec = 120000000 };

	REQUIRE(!nanosleep(&delay, NULL));
	REQUIRE(!pthread_mutex_lock(&fixture->lock));
	fixture->ready = 1;
	REQUIRE(!pthread_cond_signal(&fixture->cond));
	REQUIRE(!pthread_mutex_unlock(&fixture->lock));
	return NULL;
}

static void check_waits(void)
{
	struct cond_fixture fixture = { .lock = PTHREAD_MUTEX_INITIALIZER };
	pthread_condattr_t attr;
	pthread_t worker;
	struct timespec deadline;
	const struct timespec delay = { .tv_nsec = 120000000 };
	struct pollfd ignored = { .fd = -1, .events = POLLIN, .revents = POLLIN };
	int64_t start = monotonic_ms();

	REQUIRE(!poll(NULL, 0, 120));
	REQUIRE(monotonic_ms() - start >= 100);
	start = monotonic_ms();
	REQUIRE(!ppoll(&ignored, 1, &delay, NULL) && !ignored.revents);
	REQUIRE(monotonic_ms() - start >= 100);
	REQUIRE(!pthread_condattr_init(&attr));
	REQUIRE(!pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
	REQUIRE(!pthread_cond_init(&fixture.cond, &attr));
	REQUIRE(!pthread_condattr_destroy(&attr));
	REQUIRE(!pthread_mutex_lock(&fixture.lock));
	REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &deadline));
	start = (int64_t)deadline.tv_sec * 1000 + deadline.tv_nsec / 1000000;
	deadline.tv_nsec += 120000000;
	if (deadline.tv_nsec >= 1000000000) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000;
	}
	REQUIRE(pthread_cond_timedwait(&fixture.cond, &fixture.lock, &deadline) == ETIMEDOUT);
	REQUIRE(monotonic_ms() - start >= 100);

	REQUIRE(!pthread_create(&worker, NULL, signal_cond, &fixture));
	REQUIRE(!clock_gettime(CLOCK_MONOTONIC, &deadline));
	deadline.tv_sec += 3;
	while (!fixture.ready)
		REQUIRE(!pthread_cond_timedwait(&fixture.cond, &fixture.lock, &deadline));
	REQUIRE(!pthread_mutex_unlock(&fixture.lock));
	REQUIRE(!pthread_join(worker, NULL));
	REQUIRE(!pthread_cond_destroy(&fixture.cond));
	REQUIRE(!pthread_mutex_destroy(&fixture.lock));
	puts("media-runtime: waits ok");
}

static void check_io(void)
{
	const struct timespec timeout = { .tv_sec = 3 };
	const struct timespec delay = { .tv_nsec = 120000000 };
	int pipefd[2], pair[2], status;
	char path[] = "/root/media-runtime.XXXXXX";
	char byte;
	struct pollfd fd = { .events = POLLIN };

	REQUIRE(!pipe(pipefd));
	fd.fd = pipefd[0];
	REQUIRE(!poll(&fd, 1, 0) && !fd.revents);
	int64_t start = monotonic_ms();
	REQUIRE(!ppoll(&fd, 1, &delay, NULL) && !fd.revents);
	REQUIRE(monotonic_ms() - start >= 100);
	pid_t child = fork();
	REQUIRE(child >= 0);
	if (!child) {
		REQUIRE(!nanosleep(&delay, NULL));
		REQUIRE(write(pipefd[1], "p", 1) == 1);
		_exit(0);
	}
	REQUIRE(!close(pipefd[1]));
	REQUIRE(ppoll(&fd, 1, &timeout, NULL) == 1 && (fd.revents & POLLIN));
	REQUIRE(read(fd.fd, &byte, 1) == 1 && byte == 'p');
	REQUIRE(waitpid(child, &status, 0) == child && status == 0);
	REQUIRE(read(fd.fd, &byte, 1) == 0);
	REQUIRE(!close(fd.fd));

	REQUIRE(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
	REQUIRE(send(pair[1], "s", 1, 0) == 1);
	fd.fd = pair[0];
	REQUIRE(poll(&fd, 1, 0) == 1 && (fd.revents & POLLIN));
	REQUIRE(recv(fd.fd, &byte, 1, 0) == 1 && byte == 's');
	REQUIRE(!close(pair[1]));
	REQUIRE(recv(fd.fd, &byte, 1, 0) == 0);
	REQUIRE(!close(fd.fd));

	int file = mkstemp(path);
	REQUIRE(file >= 0);
	REQUIRE(write(file, "abc", 3) == 3);
	REQUIRE(lseek(file, -2, SEEK_END) == 1);
	REQUIRE(read(file, &byte, 1) == 1 && byte == 'b');
	REQUIRE(lseek(file, 0, SEEK_END) == 3 && read(file, &byte, 1) == 0);
	REQUIRE(!close(file) && !unlink(path));
	puts("media-runtime: io ok");
}

int main(int argc, char **argv)
{
	REQUIRE(argc == 2);
	if (!strcmp(argv[1], "context"))
		check_context();
	else if (!strcmp(argv[1], "exec")) {
		x87_empty();
		puts("media-runtime: context and exec ok");
	} else if (!strcmp(argv[1], "signals"))
		check_signals();
	else if (!strcmp(argv[1], "migrate"))
		check_migration();
	else if (!strcmp(argv[1], "waits"))
		check_waits();
	else if (!strcmp(argv[1], "io"))
		check_io();
	else
		REQUIRE(0);
	return 0;
}
