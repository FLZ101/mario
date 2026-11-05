#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

#include <gui/event.h>

typedef struct {
	char *order;
	int count;
} callback_log;

typedef struct {
	callback_log *log;
	char entry;
} fd_log;

typedef struct {
	gui_event_loop *loop;
	gui_event_handle handle;
	int fd;
	int calls;
} self_removing_fd;

static void append(callback_log *log, char entry)
{
	log->order[log->count++] = entry;
}

static void fd_callback(void *context, short revents)
{
	fd_log *state = context;

	assert(revents & POLLIN);
	append(state->log, state->entry);
}

static void timer_callback(void *context)
{
	append(context, 't');
}

static void self_removing_callback(void *context, short revents)
{
	self_removing_fd *state = context;
	char byte;

	assert(revents & POLLIN);
	assert(read(state->fd, &byte, 1) == 1);
	assert(gui_event_loop_remove(state->loop, state->handle));
	state->calls++;
}

static void test_wakeup_and_timer_order(void)
{
	int first_pipe[2];
	int second_pipe[2];
	uint64_t now;
	char order[3] = { 0 };
	callback_log log = { .order = order };
	fd_log first = { .log = &log, .entry = 'f' };
	fd_log second = { .log = &log, .entry = 's' };
	gui_event_loop *loop = gui_event_loop_create();

	assert(loop != NULL);
	assert(pipe(first_pipe) == 0);
	assert(pipe(second_pipe) == 0);
	assert(gui_event_loop_now_msec(&now) == 0);
	assert(gui_event_loop_add_timer(loop, now, timer_callback, &log));
	assert(gui_event_loop_add_fd(loop, first_pipe[0], POLLIN, fd_callback,
					     &first));
	assert(gui_event_loop_add_fd(loop, second_pipe[0], POLLIN, fd_callback,
					     &second));
	assert(write(first_pipe[1], "x", 1) == 1);
	assert(write(second_pipe[1], "x", 1) == 1);
	assert(gui_event_loop_run_once(loop) == 3);
	assert(log.count == 3 && order[0] == 'f' && order[1] == 's' &&
	       order[2] == 't');
	assert(close(first_pipe[0]) == 0);
	assert(close(first_pipe[1]) == 0);
	assert(close(second_pipe[0]) == 0);
	assert(close(second_pipe[1]) == 0);
	gui_event_loop_destroy(loop);
}

static void test_self_removal(void)
{
	int pipefd[2];
	self_removing_fd state = { 0 };
	gui_event_loop *loop = gui_event_loop_create();

	assert(loop != NULL);
	assert(pipe(pipefd) == 0);
	state.loop = loop;
	state.fd = pipefd[0];
	state.handle = gui_event_loop_add_fd(loop, pipefd[0], POLLIN,
					     self_removing_callback, &state);
	assert(state.handle != GUI_EVENT_HANDLE_NONE);
	assert(write(pipefd[1], "x", 1) == 1);
	assert(gui_event_loop_run_once(loop) == 1);
	assert(state.calls == 1);
	assert(gui_event_loop_run_once(loop) == 0);
	assert(close(pipefd[0]) == 0);
	assert(close(pipefd[1]) == 0);
	gui_event_loop_destroy(loop);
}

static void test_timer_expiry(void)
{
	uint64_t now;
	char order[1] = { 0 };
	callback_log log = { .order = order };
	gui_event_loop *loop = gui_event_loop_create();

	assert(loop != NULL);
	assert(gui_event_loop_now_msec(&now) == 0);
	assert(gui_event_loop_add_timer(loop, now, timer_callback, &log));
	assert(gui_event_loop_run_once(loop) == 1);
	assert(log.count == 1 && order[0] == 't');
	gui_event_loop_destroy(loop);
}

int main(void)
{
	test_wakeup_and_timer_order();
	test_self_removal();
	test_timer_expiry();
	puts("gui-event-loop: wakeup ok");
	puts("gui-event-loop: timer order ok");
	puts("gui-event-loop: timer expiry ok");
	puts("gui-event-loop: self removal ok");
	return 0;
}
