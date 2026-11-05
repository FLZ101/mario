#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include <gui/event.h>

typedef enum {
	GUI_EVENT_SOURCE_FD,
	GUI_EVENT_SOURCE_TIMER,
} gui_event_source_type;

typedef struct {
	gui_event_handle handle;
	uint64_t order;
	uint64_t ready_epoch;
	gui_event_source_type type;
	union {
		struct {
			int fd;
			short events;
			gui_event_fd_fn callback;
		} fd;
		struct {
			uint64_t deadline_msec;
			gui_event_timer_fn callback;
		} timer;
	};
	void *context;
} gui_event_source;

struct gui_event_loop {
	gui_event_source *sources;
	size_t count;
	size_t capacity;
	gui_event_handle next_handle;
	uint64_t next_order;
	uint64_t epoch;
	int dispatching;
};

static size_t gui_event_loop_find(const gui_event_loop *loop,
				  gui_event_handle handle)
{
	for (size_t index = 0; index < loop->count; index++)
		if (loop->sources[index].handle == handle)
			return index;
	return SIZE_MAX;
}

static int gui_event_loop_ready(const gui_event_source *source,
				uint64_t epoch)
{
	return source->ready_epoch <= epoch;
}

static int gui_event_loop_reserve(gui_event_loop *loop)
{
	gui_event_source *sources;
	size_t capacity;

	if (loop->count < loop->capacity)
		return 1;
	capacity = loop->capacity == 0 ? 8 : loop->capacity * 2;
	if (capacity < loop->capacity || capacity > SIZE_MAX / sizeof(*sources)) {
		errno = ENOMEM;
		return 0;
	}
	sources = realloc(loop->sources, capacity * sizeof(*sources));
	if (sources == NULL)
		return 0;
	loop->sources = sources;
	loop->capacity = capacity;
	return 1;
}

static gui_event_handle gui_event_loop_next_handle(gui_event_loop *loop)
{
	gui_event_handle handle;

	for (size_t attempts = 0; attempts <= loop->count; attempts++) {
		handle = ++loop->next_handle;
		if (handle != GUI_EVENT_HANDLE_NONE &&
		    gui_event_loop_find(loop, handle) == SIZE_MAX)
			return handle;
	}
	errno = ENOMEM;
	return GUI_EVENT_HANDLE_NONE;
}

static gui_event_source *gui_event_loop_add_source(gui_event_loop *loop,
						gui_event_source_type type,
						void *context)
{
	gui_event_source *source;
	gui_event_handle handle;

	if (loop == NULL || !gui_event_loop_reserve(loop))
		return NULL;
	handle = gui_event_loop_next_handle(loop);
	if (handle == GUI_EVENT_HANDLE_NONE)
		return NULL;
	source = &loop->sources[loop->count++];
	*source = (gui_event_source) {
		.handle = handle,
		.order = ++loop->next_order,
		.ready_epoch = loop->dispatching ? loop->epoch + 1 : loop->epoch,
		.type = type,
		.context = context,
	};
	return source;
}

static size_t gui_event_loop_next_source(const gui_event_loop *loop,
					  gui_event_source_type type,
					  uint64_t epoch, uint64_t after_order,
					  uint64_t now_msec, int due_only)
{
	size_t selected = SIZE_MAX;

	for (size_t index = 0; index < loop->count; index++) {
		const gui_event_source *source = &loop->sources[index];

		if (source->type != type || !gui_event_loop_ready(source, epoch) ||
		    source->order <= after_order ||
		    (due_only && source->timer.deadline_msec > now_msec))
			continue;
		if (selected == SIZE_MAX ||
		    source->order < loop->sources[selected].order)
			selected = index;
	}
	return selected;
}

static int gui_event_loop_timeout(const gui_event_loop *loop, uint64_t epoch,
				  uint64_t now_msec, int *has_timer)
{
	uint64_t deadline = UINT64_MAX;

	*has_timer = 0;
	for (size_t index = 0; index < loop->count; index++) {
		const gui_event_source *source = &loop->sources[index];

		if (source->type != GUI_EVENT_SOURCE_TIMER ||
		    !gui_event_loop_ready(source, epoch))
			continue;
		*has_timer = 1;
		if (source->timer.deadline_msec < deadline)
			deadline = source->timer.deadline_msec;
	}
	if (!*has_timer)
		return -1;
	if (deadline <= now_msec)
		return 0;
	if (deadline - now_msec > INT_MAX)
		return INT_MAX;
	return (int)(deadline - now_msec);
}

gui_event_loop *gui_event_loop_create(void)
{
	return calloc(1, sizeof(gui_event_loop));
}

void gui_event_loop_destroy(gui_event_loop *loop)
{
	if (loop == NULL)
		return;
	free(loop->sources);
	free(loop);
}

int gui_event_loop_now_msec(uint64_t *msec)
{
	struct timespec time;

	if (msec == NULL) {
		errno = EINVAL;
		return -1;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &time) < 0)
		return -1;
	*msec = (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
	return 0;
}

gui_event_handle gui_event_loop_add_fd(gui_event_loop *loop, int fd,
				       short events, gui_event_fd_fn callback,
				       void *context)
{
	gui_event_source *source;

	if (fd < 0 || events == 0 || callback == NULL) {
		errno = EINVAL;
		return GUI_EVENT_HANDLE_NONE;
	}
	source = gui_event_loop_add_source(loop, GUI_EVENT_SOURCE_FD, context);
	if (source == NULL)
		return GUI_EVENT_HANDLE_NONE;
	source->fd.fd = fd;
	source->fd.events = events;
	source->fd.callback = callback;
	return source->handle;
}

gui_event_handle gui_event_loop_add_timer(gui_event_loop *loop,
					  uint64_t deadline_msec,
					  gui_event_timer_fn callback,
					  void *context)
{
	gui_event_source *source;

	if (callback == NULL) {
		errno = EINVAL;
		return GUI_EVENT_HANDLE_NONE;
	}
	source = gui_event_loop_add_source(loop, GUI_EVENT_SOURCE_TIMER, context);
	if (source == NULL)
		return GUI_EVENT_HANDLE_NONE;
	source->timer.deadline_msec = deadline_msec;
	source->timer.callback = callback;
	return source->handle;
}

int gui_event_loop_remove(gui_event_loop *loop, gui_event_handle handle)
{
	size_t index;

	if (loop == NULL || handle == GUI_EVENT_HANDLE_NONE)
		return 0;
	index = gui_event_loop_find(loop, handle);
	if (index == SIZE_MAX)
		return 0;
	loop->sources[index] = loop->sources[--loop->count];
	return 1;
}

int gui_event_loop_run_once(gui_event_loop *loop)
{
	struct pollfd *pollfds = NULL;
	gui_event_handle *handles = NULL;
	size_t fd_count = 0;
	uint64_t fd_order = 0;
	uint64_t now_msec;
	uint64_t timer_order;
	uint64_t epoch;
	int callbacks = 0;
	int has_timer;
	int poll_result;

	if (loop == NULL) {
		errno = EINVAL;
		return -1;
	}
	epoch = ++loop->epoch;
	loop->dispatching = 1;
	for (;;) {
		size_t index = gui_event_loop_next_source(loop, GUI_EVENT_SOURCE_FD,
						  epoch, fd_order, 0, 0);

		if (index == SIZE_MAX)
			break;
		fd_count++;
		fd_order = loop->sources[index].order;
	}
	if (fd_count != 0) {
		pollfds = calloc(fd_count, sizeof(*pollfds));
		handles = calloc(fd_count, sizeof(*handles));
		if (pollfds == NULL || handles == NULL) {
			errno = ENOMEM;
			goto error;
		}
		fd_order = 0;
		for (size_t count = 0; count < fd_count; count++) {
			size_t index = gui_event_loop_next_source(loop,
							  GUI_EVENT_SOURCE_FD, epoch,
							  fd_order, 0, 0);
			const gui_event_source *source = &loop->sources[index];

			pollfds[count].fd = source->fd.fd;
			pollfds[count].events = source->fd.events;
			handles[count] = source->handle;
			fd_order = source->order;
		}
	}
	for (;;) {
		int timeout;

		if (gui_event_loop_now_msec(&now_msec) < 0)
			goto error;
		timeout = gui_event_loop_timeout(loop, epoch, now_msec, &has_timer);
		if (fd_count == 0 && !has_timer)
			goto done;
		for (size_t index = 0; index < fd_count; index++)
			pollfds[index].revents = 0;
		poll_result = poll(pollfds, fd_count, timeout);
		if (poll_result >= 0)
			break;
		if (errno != EINTR)
			goto error;
	}
	for (size_t count = 0; count < fd_count; count++) {
		size_t index;
		gui_event_source source;

		if (pollfds[count].revents == 0)
			continue;
		index = gui_event_loop_find(loop, handles[count]);
		if (index == SIZE_MAX || loop->sources[index].type != GUI_EVENT_SOURCE_FD ||
		    !gui_event_loop_ready(&loop->sources[index], epoch))
			continue;
		/* Re-find by handle so a prior callback can safely remove this source. */
		source = loop->sources[index];
		source.fd.callback(source.context, pollfds[count].revents);
		callbacks++;
	}
	if (gui_event_loop_timeout(loop, epoch, 0, &has_timer) != -1 &&
	    gui_event_loop_now_msec(&now_msec) < 0)
		goto error;
	timer_order = 0;
	for (;;) {
		size_t index = gui_event_loop_next_source(loop, GUI_EVENT_SOURCE_TIMER,
						  epoch, timer_order, now_msec, 1);
		gui_event_source source;

		if (index == SIZE_MAX)
			break;
		source = loop->sources[index];
		timer_order = source.order;
		gui_event_loop_remove(loop, source.handle);
		source.timer.callback(source.context);
		callbacks++;
	}

done:
	loop->dispatching = 0;
	free(handles);
	free(pollfds);
	return callbacks;

error:
	loop->dispatching = 0;
	free(handles);
	free(pollfds);
	return -1;
}
