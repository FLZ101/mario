#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mario/input.h>
#include <gui/surface.h>
#include "../../usr/terminal/upload.c"

static gui_flush_result scanout_result;
static unsigned int scanout_calls;
static int fail_surface_allocation;
static int fail_record_allocation;
static int use_test_clock;
static uint64_t test_clock_ms;
static int busy_input;
static unsigned int input_reads;

static int test_clock_gettime(clockid_t clock, struct timespec *now)
{
	if (!use_test_clock)
		return clock_gettime(clock, now);
	assert(clock == CLOCK_MONOTONIC);
	now->tv_sec = test_clock_ms / 1000;
	now->tv_nsec = (test_clock_ms % 1000) * 1000000;
	return 0;
}

static ssize_t test_input_read(int descriptor, void *buffer, size_t bytes)
{
	if (!busy_input || (descriptor != 900 && descriptor != 901))
		return read(descriptor, buffer, bytes);
	input_reads++;
	test_clock_ms++;
	if (descriptor == 900) {
		struct mario_psaux_event *events = buffer;

		assert(bytes == 16 * sizeof(*events));
		for (unsigned int index = 0; index < 16; index++)
			events[index] = (struct mario_psaux_event) { .dx = 1 };
	} else {
		struct mario_gui_key_event *events = buffer;

		assert(bytes == 16 * sizeof(*events));
		for (unsigned int index = 0; index < 16; index++)
			events[index] = (struct mario_gui_key_event) { .scancode = 0x1e, .pressed = 1 };
	}
	return bytes;
}

typedef struct {
	uint64_t filled_pixels;
	uint64_t blitted_pixels;
	uint64_t flush_requests;
	uint64_t writer_calls;
	uint64_t written_bytes;
} drawing_work;

static drawing_work measured_work;
static int measure_drawing;

static uint64_t drawing_area(const gui_surface *surface, int64_t x, int64_t y,
			    int64_t width, int64_t height)
{
	int64_t right = x + width;
	int64_t bottom = y + height;

	if (surface == NULL || surface->pixels == NULL || width <= 0 || height <= 0)
		return 0;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (right > surface->width)
		right = surface->width;
	if (bottom > surface->height)
		bottom = surface->height;
	return right > x && bottom > y ? (uint64_t)(right - x) * (bottom - y) : 0;
}

static void measured_fill(gui_surface *surface, gui_rect rectangle, gui_color color)
{
	if (measure_drawing)
		measured_work.filled_pixels += drawing_area(surface, rectangle.x,
			rectangle.y, rectangle.w, rectangle.h);
	gui_fill_rect(surface, rectangle, color);
}

static void measured_blit(gui_surface *destination, int destination_x,
			  int destination_y, const gui_surface *source,
			  const gui_rect *rectangle)
{
	if (measure_drawing && source != NULL && rectangle != NULL) {
		int64_t left = rectangle->x > 0 ? rectangle->x : 0;
		int64_t top = rectangle->y > 0 ? rectangle->y : 0;
		int64_t right = (int64_t)rectangle->x + rectangle->w;
		int64_t bottom = (int64_t)rectangle->y + rectangle->h;

		if (right > source->width)
			right = source->width;
		if (bottom > source->height)
			bottom = source->height;
		if (source->pixels != NULL && rectangle->w > 0 && rectangle->h > 0)
			measured_work.blitted_pixels += drawing_area(destination,
				(int64_t)destination_x + left - rectangle->x,
				(int64_t)destination_y + top - rectangle->y,
				right - left, bottom - top);
	}
	gui_blit(destination, destination_x, destination_y, source, rectangle);
}

static ssize_t measured_write(void *context, const void *bytes, size_t count,
			      off_t offset)
{
	(void)context;
	assert(bytes != NULL && offset >= 0 && count > 0);
	assert((uint64_t)offset + count <= 1024 * 768 * 4);
	measured_work.writer_calls++;
	measured_work.written_bytes += count;
	return (ssize_t)count;
}

static void *test_record_create(size_t count, size_t bytes)
{
	if (fail_record_allocation) {
		fail_record_allocation = 0;
		errno = ENOMEM;
		return NULL;
	}
	return calloc(count, bytes);
}

static gui_surface *test_surface_create(int width, int height)
{
	if (fail_surface_allocation) {
		fail_surface_allocation = 0;
		return NULL;
	}
	return gui_surface_create(width, height);
}

static gui_flush_result test_surface_flush_rect_fd(const gui_surface *surface,
		gui_rect rectangle, size_t stride, int descriptor)
{
	assert(surface != NULL && rectangle.x >= 0 && rectangle.y >= 0 && rectangle.w > 0 && rectangle.h > 0);
	assert(rectangle.x + rectangle.w <= surface->width && rectangle.y + rectangle.h <= surface->height);
	assert(stride == (size_t)surface->width * 4 && descriptor == 123);
	scanout_calls++;
	if (measure_drawing) {
		measured_work.flush_requests++;
		assert(scanout_result == GUI_FLUSH_COMPLETE);
		return gui_surface_flush_rect(surface, rectangle, stride,
			measured_write, NULL);
	}
	return scanout_result;
}

#define main wm_program_main
#define gui_surface_create test_surface_create
#define calloc test_record_create
#define gui_fill_rect measured_fill
#define gui_blit measured_blit
#define gui_surface_flush_rect_fd test_surface_flush_rect_fd
#define clock_gettime test_clock_gettime
#define read test_input_read
#include "../../gui/desktop/mario-wm/main.c"
#undef read
#undef clock_gettime
#undef gui_surface_flush_rect_fd
#undef gui_blit
#undef gui_fill_rect
#undef gui_surface_create
#undef calloc
#undef main

#define main desktop_program_main
#include "../../gui/desktop/mario-desktop/main.c"
#undef main

static void init_server(wm_server *server, uint32_t width, uint32_t height);
static wm_window *init_client(wm_server *server, size_t index, gui_protocol_endpoint_role role);
static void cleanup_server(wm_server *server);

static void test_commit(gui_flush_result result, int graphics)
{
	static wm_server server;
	wm_window *client;
	gui_surface *previous = gui_surface_create(8, 8);
	gui_surface *next = gui_surface_create(8, 8);
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	assert(previous != NULL && next != NULL);
	init_server(&server, 8, 8);
	client = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	server.state = (wm_state) { .framebuffer = 123, .graphics = graphics,
		.width = 8, .height = 8, .scanout_stride = 32, .surface_bytes = 512,
		.frame_number = 7, .frame_bytes = 256, .surface_limit = 4096,
		.composite = gui_surface_create(8, 8) };
	assert(server.state.composite != NULL);
	client->width = 8;
	client->height = 8;
	client->x = 0;
	client->y = 0;
	client->committed = previous;
	client->committed_bytes = 256;
	client->staging = next;
	client->staging_bytes = 256;
	client->staging_pixels = 64;
	client->staging_frame_serial = 2;
	client->client->surface_bytes = 512;
	put_u32(commit, 1);
	put_u64(commit + 4, 1);
	put_u64(commit + 12, 2);
	scanout_result = result;
	scanout_calls = 0;
	assert(client_commit_surface(client, commit) == 0);
	assert(scanout_calls == (unsigned int)(graphics && result == GUI_FLUSH_ERROR ? 2 : graphics));
	assert(server.state.frame_number == 7);
	if (result == GUI_FLUSH_ERROR && graphics) {
		assert(client->committed == previous && client->staging == NULL);
		assert(client->configure_serial == 2 && server.state.surface_bytes == 256);
		assert(gui_protocol_output_pending(client->client->output));
	} else {
		assert(client->committed == next && client->staging == NULL);
		assert(client->staging_frame_serial == 0 && client->staging_pixels == 0);
		assert(server.state.surface_bytes == 256);
		assert(!gui_protocol_output_pending(client->client->output));
	}
	assert(client->client->surface_bytes == 256);
	cleanup_server(&server);
}

static uint8_t captured_output[4096];
static size_t captured_bytes;

static ssize_t capture_output(void *context, const void *bytes, size_t count)
{
	(void)context;
	assert(count <= sizeof(captured_output) - captured_bytes);
	memcpy(captured_output + captured_bytes, bytes, count);
	captured_bytes += count;
	return (ssize_t)count;
}

static void drain_output(wm_window *client)
{
	captured_bytes = 0;
	assert(gui_protocol_output_flush(client->client->output, capture_output, NULL) ==
		GUI_PROTOCOL_OUTPUT_EMPTY);
}

static void init_server(wm_server *server, uint32_t width, uint32_t height)
{
	memset(server, 0, sizeof(*server));
	server->state.width = width;
	server->state.height = height;
	assert(surface_bytes(width, height, &server->state.frame_bytes) == 0);
	server->state.surface_limit = app_gui_surface_budget(server->state.frame_bytes);
}

static wm_window *init_client(wm_server *server, size_t index,
				gui_protocol_endpoint_role role)
{
	wm_client *peer = wm_add_client(server, dup(STDOUT_FILENO), role);
	wm_window *window;

	assert(peer != NULL);
	peer->hello = 1;
	window = wm_record_create(server, peer, sizeof(*window));
	assert(window != NULL);
	*window = (wm_window) { .client = peer, .server = server, .configure_serial = 1,
		.window_id = (uint32_t)index + 1, .width = 64, .height = 48, .x = 64, .y = 64,
		.next = server->windows };
	server->windows = window;
	peer->windows = window;
	if (server->next_window_id < window->window_id)
		server->next_window_id = window->window_id;
	return window;
}

static void cleanup_server(wm_server *server)
{
	wm_discard_shell_list(server);
	while (server->clients != NULL)
		client_destroy(server->clients);
	assert(wm_poll_resize(server, 0) == 0);
	assert(server->record_bytes == 0 && server->windows == NULL && server->client_count == 0);
	if (server->desktop_snapshot != NULL) {
		server->state.surface_bytes -= server->desktop_snapshot_bytes;
		gui_surface_destroy(server->desktop_snapshot);
	}
	assert(server->state.surface_bytes == 0);
	gui_surface_destroy(server->state.composite);
}

static void stage(wm_window *client, uint64_t serial, uint64_t frame)
{
	uint8_t begin[sizeof(gui_protocol_wire_surface_begin)] = { 0 };

	put_u32(begin, client->window_id);
	put_u64(begin + 4, serial);
	put_u64(begin + 12, frame);
	put_u32(begin + 28, client->width);
	put_u32(begin + 32, client->height);
	assert(client_stage_surface(client, begin) == 0);
}

static void commit_staged(wm_window *client)
{
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	assert(client->staging != NULL);
	client->staging_pixels = (size_t)client->width * client->height;
	put_u32(commit, client->window_id);
	put_u64(commit + 4, client->configure_serial);
	put_u64(commit + 12, client->staging_frame_serial);
	assert(client_commit_surface(client, commit) == 0);
}

static void test_status_readers(void)
{
	wm_state state = { .frame_number = 17 };
	int descriptor;
	int synchronization[2];
	pid_t child;
	int status;
	int result;
	int saved_errno;
	char marker[32] = { 0 };
	char ready;

	unlink(WM_FRAME_PATH);
	unlink(WM_READY_PATH);
	assert(wm_publish_completed_frame(&state) == 0 && state.frame_number == 18);
	descriptor = open(WM_FRAME_PATH, O_RDONLY);
	assert(descriptor >= 0 && pipe(synchronization) == 0);
	child = fork();
	assert(child >= 0);
	if (child == 0) {
		close(synchronization[0]);
		if (write(synchronization[1], "r", 1) != 1 || usleep(40000) < 0 ||
		    read(descriptor, marker, sizeof(marker)) != 3 || strcmp(marker, "18\n") != 0)
			_exit(2);
		close(descriptor);
		close(synchronization[1]);
		_exit(0);
	}
	close(descriptor);
	close(synchronization[1]);
	assert(read(synchronization[0], &ready, 1) == 1 && ready == 'r');
	close(synchronization[0]);
	result = wm_publish_completed_frame(&state);
	saved_errno = errno;
	assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	if (result < 0) {
		fprintf(stderr, "gui-wm-busy: transient marker reader rejected: %d\n", saved_errno);
		_exit(1);
	}
	assert(state.frame_number == 19);
	descriptor = open(WM_FRAME_PATH, O_RDONLY);
	assert(descriptor >= 0);
	assert(wm_publish_completed_frame(&state) < 0 && errno == EBUSY);
	assert(state.frame_number == 19 && access(WM_FRAME_PATH ".new", F_OK) < 0);
	memset(marker, 0, sizeof(marker));
	assert(read(descriptor, marker, sizeof(marker)) == 3 && strcmp(marker, "19\n") == 0);
	close(descriptor);
	unlink(WM_FRAME_PATH);
	unlink(WM_READY_PATH);
}

static void test_occlusion(void)
{
	static wm_server server;
	wm_window *target;
	wm_window *left;
	wm_window *right;
	gui_surface *latest;
	size_t retained = 320 * 192 * 4;
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	init_server(&server, 1024, 768);
	server.state.framebuffer = 123;
	server.state.scanout_stride = 1024 * 4;
	server.state.composite = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL);
	server.state.graphics = 1;
	server.state.frame_number = 17;
	target = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	left = init_client(&server, 1, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	right = init_client(&server, 2, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	target->width = 320;
	target->height = 192;
	target->x = 64;
	target->y = 96;
	target->stack_order = 1;
	left->width = right->width = 160;
	left->height = right->height = 192;
	left->x = 64;
	right->x = 224;
	left->y = right->y = 96;
	left->stack_order = 2;
	right->stack_order = 3;
	assert(wm_window_obscured(&server, target));
	assert(!wm_window_obscured(&server, left) && !wm_window_obscured(&server, right));
	right->x += 2 * WM_RESIZE_GRIP + 1;
	assert(!wm_window_obscured(&server, target));
	right->x -= 3;
	right->y++;
	assert(!wm_window_obscured(&server, target));
	right->y--;
	right->stack_order = 0;
	assert(!wm_window_obscured(&server, target));
	right->stack_order = 3;
	target->committed = gui_surface_create(320, 192);
	assert(target->committed != NULL);
	gui_fill_rect(target->committed, (gui_rect) { 0, 0, 320, 192 }, 0x00225688);
	target->committed_bytes = retained;
	target->client->surface_bytes = retained;
	server.state.surface_bytes = retained;
	stage(target, 1, 2);
	latest = target->staging;
	assert(latest != NULL && server.state.surface_bytes == 2 * retained);
	gui_fill_rect(latest, (gui_rect) { 0, 0, 320, 192 }, 0x00448866);
	scanout_calls = 0;
	scanout_result = GUI_FLUSH_ERROR;
	commit_staged(target);
	assert(scanout_calls == 0 && server.state.frame_number == 17);
	assert(target->committed == latest && target->staging == NULL);
	assert(server.state.surface_bytes == retained && target->client->surface_bytes == retained);
	stage(target, 1, 3);
	assert(target->staging != NULL);
	target->staging->pixels[0] = 0x00ffffff;
	target->staging_pixels = 1;
	put_u32(commit, target->window_id);
	put_u64(commit + 4, 1);
	put_u64(commit + 12, 3);
	assert(client_commit_surface(target, commit) == 0);
	assert(scanout_calls == 0 && target->committed == latest);
	assert(target->staging_pixels == 1 && server.state.surface_bytes == 2 * retained);
	client_discard_staging(target);
	fail_surface_allocation = 1;
	stage(target, 1, 4);
	assert(target->committed == latest && target->staging == NULL);
	assert(scanout_calls == 0 && server.state.surface_bytes == retained);
	right->x = 600;
	assert(!wm_window_obscured(&server, target));
	scanout_result = GUI_FLUSH_BUSY;
	assert(wm_compose(&server) == GUI_FLUSH_BUSY);
	assert(scanout_calls == 1 && server.state.frame_number == 17);
	assert(server.state.composite->pixels[100 * 1024 + 300] == 0x00448866);
	window_destroy(left);
	assert(wm_compose(&server) == GUI_FLUSH_BUSY);
	assert(server.state.composite->pixels[100 * 1024 + 64] == 0x00448866);
	server.state.graphics = 0;
	stage(target, 2, 5);
	gui_fill_rect(target->staging, (gui_rect) { 0, 0, 320, 192 }, 0x00dd8844);
	commit_staged(target);
	assert(scanout_calls == 2 && server.state.surface_bytes == retained);
	server.state.graphics = 1;
	assert(wm_compose(&server) == GUI_FLUSH_BUSY);
	assert(scanout_calls == 3 && server.state.composite->pixels[100 * 1024 + 64] == 0x00dd8844);
	cleanup_server(&server);
}

static void fullscreen_stage(wm_window *window, gui_color color)
{
	stage(window, window->configure_serial, window->configure_serial);
	assert(window->staging != NULL);
	gui_fill_rect(window->staging, (gui_rect) { 0, 0, window->width, window->height }, color);
}

static void test_fullscreen(void)
{
	static wm_server server;
	wm_window *target;
	wm_window *sibling;
	wm_window *automatic;
	wm_window *desktop;
	wm_window *cover;
	wm_client *peer;
	gui_surface *previous;
	size_t normal_bytes = 320 * 192 * 4;
	size_t sibling_bytes = 128 * 96 * 4;
	size_t surface_limit;
	uint8_t create[sizeof(gui_protocol_wire_window_create)] = { 0 };
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };
	gui_protocol_header destroy = { .type = GUI_PROTOCOL_WINDOW_DESTROY };
	gui_protocol_header activate = { .type = GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW };
	gui_input_event key = { .type = GUI_INPUT_EVENT_KEY,
		.key = { .scancode = 0x1e, .pressed = 1 } };
	wm_hit_region region;

	init_server(&server, 1024, 768);
	server.state.framebuffer = 123;
	server.state.scanout_stride = 1024 * 4;
	server.state.composite = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL);
	scanout_result = GUI_FLUSH_BUSY;
	target = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	target->width = 320;
	target->height = 192;
	target->x = 144;
	target->y = 112;
	fullscreen_stage(target, 0x00225688);
	commit_staged(target);
	peer = target->client;
	put_u32(create, 100);
	put_u32(create + 4, 128);
	put_u32(create + 8, 96);
	client_create_window(peer, create, sizeof(create), 0);
	sibling = peer->windows;
	assert(sibling != target && sibling->window_id == 2);
	sibling->x = 680;
	sibling->y = 112;
	fullscreen_stage(sibling, 0x00dd8844);
	commit_staged(sibling);
	wm_set_input_owner(&server, target);
	drain_output(target);
	server.state.graphics = 1;
	surface_limit = server.state.surface_limit;
	server.state.surface_limit = normal_bytes + sibling_bytes;
	assert(wm_toggle_fullscreen(&server, target) == -ENOMEM);
	assert(!target->mode_pending && !target->fullscreen && target->configure_serial == 1);
	assert(server.fullscreen_window == NULL && server.state.surface_bytes == normal_bytes + sibling_bytes);
	server.state.surface_limit = surface_limit;
	previous = target->committed;
	assert(wm_toggle_fullscreen(&server, target) == 1);
	assert(target->mode_pending && target->fullscreen && target->configure_serial == 2);
	assert(target->width == 1024 && target->height == 768 && target->x == 0 && target->y == 0);
	assert(target->normal_x == 144 && target->normal_y == 112);
	assert(target->normal_width == 320 && target->normal_height == 192);
	assert(wm_toggle_fullscreen(&server, target) == -EBUSY);
	drain_output(target);
	wm_emit_input(&server, &key);
	assert(!gui_protocol_output_pending(peer->output));
	assert(wm_compose(&server) == GUI_FLUSH_BUSY);
	assert(server.state.composite->pixels[112 * 1024 + 144] == 0x00225688);
	fail_surface_allocation = 1;
	stage(target, 2, 2);
	assert(target->configure_serial == 3 && !target->mode_pending && !target->fullscreen);
	assert(target->x == 144 && target->y == 112 && target->width == 320 && target->height == 192);
	assert(target->committed == previous && target->reserved_bytes == 0);
	assert(server.fullscreen_window == NULL && server.state.surface_bytes == normal_bytes + sibling_bytes);
	drain_output(target);
	assert(wm_toggle_fullscreen(&server, target) == 1);
	fullscreen_stage(target, 0x00448866);
	scanout_result = GUI_FLUSH_ERROR;
	commit_staged(target);
	assert(target->configure_serial == 5 && target->committed == previous);
	assert(!target->fullscreen && !target->mode_pending && server.fullscreen_window == NULL);
	assert(target->x == 144 && target->y == 112 && target->width == 320 && target->height == 192);
	assert(server.state.surface_bytes == normal_bytes + sibling_bytes);
	scanout_result = GUI_FLUSH_BUSY;
	drain_output(target);
	assert(wm_toggle_fullscreen(&server, target) == 1);
	fullscreen_stage(target, 0x00448866);
	commit_staged(target);
	previous = target->committed;
	assert(target->configure_serial == 6 && target->fullscreen && !target->mode_pending);
	assert(server.fullscreen_window == target && server.input_owner == target);
	assert(server.state.surface_bytes == server.state.frame_bytes + sibling_bytes);
	assert(server.state.composite->pixels[0] == 0x00448866);
	assert(server.state.composite->pixels[768 * 1024 - 1] == 0x00448866);
	assert(wm_hit_test(&server, 0, 0, &region) == target && region == WM_HIT_CONTENT);
	assert(wm_hit_test(&server, 1023, 767, &region) == target && region == WM_HIT_CONTENT);
	assert(wm_move_window(&server, target, 40, 60) == -EBUSY);
	assert(wm_resize_window(&server, target, 0, 0, 64, 48) == -EBUSY);
	drain_output(target);
	put_u32(create, 101);
	put_u32(create + 4, 64);
	put_u32(create + 8, 48);
	client_create_window(peer, create, sizeof(create), 0);
	automatic = peer->windows;
	assert(automatic != sibling && automatic != target);
	assert(server.input_owner == target && server.deferred_focus_id == 0);
	assert(target->stack_order > automatic->stack_order);
	drain_output(target);
	assert(captured_bytes == GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create_reply) +
		2 * GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_focus));
	assert(get_u16(captured_output + 36) == GUI_PROTOCOL_WINDOW_FOCUS);
	assert(get_u32(captured_output + 40) == automatic->window_id && get_u32(captured_output + 44) == 0);
	assert(get_u16(captured_output + 52) == GUI_PROTOCOL_INPUT_RESET);
	window_destroy(automatic);
	desktop = init_client(&server, server.next_window_id, GUI_PROTOCOL_ENDPOINT_SHELL);
	assert(wm_toggle_fullscreen(&server, desktop) == 0 && server.fullscreen_window == target);
	wm_set_input_owner(&server, sibling);
	assert(target->configure_serial == 7 && target->mode_pending && !target->fullscreen);
	assert(server.input_owner == target && server.fullscreen_window == target);
	assert(server.deferred_focus_id == sibling->window_id && !sibling->focused);
	assert(wm_compose(&server) == GUI_FLUSH_BUSY && server.state.composite->pixels[0] == 0x00448866);
	fail_surface_allocation = 1;
	stage(target, 7, 7);
	assert(target->configure_serial == 8 && target->fullscreen && !target->mode_pending);
	assert(server.fullscreen_window == target && server.input_owner == target);
	assert(server.deferred_focus_id == 0 && target->committed == previous);
	assert(target->width == 1024 && target->height == 768 && target->x == 0 && target->y == 0);
	wm_set_input_owner(&server, sibling);
	fullscreen_stage(target, 0x00225688);
	scanout_result = GUI_FLUSH_ERROR;
	commit_staged(target);
	assert(target->configure_serial == 10 && target->fullscreen && target->committed == previous);
	assert(server.fullscreen_window == target && server.input_owner == target && !sibling->focused);
	assert(server.deferred_focus_id == 0 && server.state.surface_bytes == server.state.frame_bytes + sibling_bytes);
	scanout_result = GUI_FLUSH_BUSY;
	drain_output(target);
	put_u32(commit, sibling->window_id);
	client_message(desktop->client, &activate, commit, 4);
	assert(target->configure_serial == 11 && target->mode_pending);
	stage(target, 10, 10);
	assert(target->staging == NULL && target->mode_pending && target->committed == previous);
	fullscreen_stage(target, 0x00225688);
	cover = init_client(&server, server.next_window_id, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	cover->width = 1024;
	cover->height = 768;
	cover->x = 0;
	cover->y = 25;
	cover->stack_order = ++server.next_stack_order;
	target->staging_pixels = 1;
	put_u32(commit, target->window_id);
	put_u64(commit + 4, 11);
	put_u64(commit + 12, 11);
	assert(client_commit_surface(target, commit) == 0);
	assert(target->mode_pending && target->committed == previous && server.input_owner == target);
	scanout_calls = 0;
	commit_staged(target);
	assert(scanout_calls == 2 && server.state.composite->pixels[0] == WM_WINDOW_BORDER_COLOR);
	assert(wm_window_obscured(&server, target));
	window_destroy(cover);
	assert(!target->fullscreen && !target->mode_pending && server.fullscreen_window == NULL);
	assert(server.input_owner == sibling && sibling->focused && !target->focused);
	assert(target->x == 144 && target->y == 112 && target->width == 320 && target->height == 192);
	assert(server.state.surface_bytes == normal_bytes + sibling_bytes && peer->surface_bytes == normal_bytes + sibling_bytes);
	wm_set_input_owner(&server, target);
	drain_output(target);
	assert(wm_toggle_fullscreen(&server, target) == 1);
	server.state.graphics = 0;
	fullscreen_stage(target, 0x00448866);
	scanout_calls = 0;
	commit_staged(target);
	assert(scanout_calls == 0 && target->fullscreen && !target->mode_pending);
	server.state.graphics = 1;
	assert(wm_compose(&server) == GUI_FLUSH_BUSY && server.state.composite->pixels[0] == 0x00448866);
	put_u32(commit, target->window_id);
	client_message(peer, &destroy, commit, 4);
	assert(server.fullscreen_window == NULL && server.input_owner == sibling);
	assert(peer->windows == sibling && peer->hello == 1 && peer->surface_bytes == sibling_bytes);
	assert(wm_toggle_fullscreen(&server, desktop) == 0 && server.fullscreen_window == NULL);
	assert(server.state.surface_bytes == sibling_bytes);
	cleanup_server(&server);
}

static void test_admission(void)
{
	static wm_server server;
	wm_window *ordinary;
	wm_window *shell;
	size_t frame;
	uint8_t create[sizeof(gui_protocol_wire_window_create)] = { 0 };
	gui_protocol_header header = { .type = GUI_PROTOCOL_WINDOW_CREATE };

	assert(app_gui_surface_budget(0) == 0);
	assert(app_gui_surface_budget(1) == 16);
	assert(app_gui_surface_budget(SIZE_MAX) == APP_GUI_SESSION_SURFACE_BYTES);
	assert(app_gui_surface_budget(APP_GUI_SESSION_SURFACE_BYTES / 16) ==
		APP_GUI_SESSION_SURFACE_BYTES);
	init_server(&server, 256, 192);
	frame = server.state.frame_bytes;
	ordinary = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	shell = init_client(&server, 1, GUI_PROTOCOL_ENDPOINT_SHELL);
	server.state.surface_limit = 3 * frame;
	server.desktop_snapshot = gui_surface_create(256, 192);
	assert(server.desktop_snapshot != NULL);
	server.desktop_snapshot_bytes = frame;
	server.state.surface_bytes = frame;
	assert(client_reserve_surface(ordinary, frame) == 0);
	assert(server.state.surface_bytes == 2 * frame);
	assert(!client_surface_fits(ordinary, 1, 0));
	assert(client_reserve_surface(shell, frame) == 0);
	assert(server.state.surface_bytes == server.state.surface_limit);
	ordinary->width = 256;
	ordinary->height = 192;
	stage(ordinary, 1, 1);
	assert(ordinary->staging != NULL && ordinary->reserved_bytes == 0);
	assert(server.state.surface_bytes == 3 * frame);
	client_discard_staging(ordinary);
	shell->width = 256;
	shell->height = 192;
	stage(shell, 1, 1);
	assert(shell->staging != NULL && server.state.surface_bytes == 2 * frame);
	assert(client_reserve_surface(ordinary, frame) == 0);
	commit_staged(shell);
	assert(server.desktop_snapshot == NULL && server.shell_ready);
	assert(server.state.surface_bytes == 2 * frame);
	assert(!client_surface_fits(ordinary, 1, 0));
	wm_client *peer = ordinary->client;
	client_discard_reservation(ordinary);
	window_destroy(ordinary);
	put_u32(create, 99);
	put_u32(create + 4, 64);
	put_u32(create + 8, 48);
	server.state.surface_limit = 2 * frame + 64 * 48 * 4 - 1;
	client_message(peer, &header, create, sizeof(create));
	assert(peer->windows == NULL && peer->surface_bytes == 0);
	assert(peer->hello == 1 && server.state.surface_bytes == frame);
	captured_bytes = 0;
	assert(gui_protocol_output_flush(peer->output, capture_output, NULL) == GUI_PROTOCOL_OUTPUT_EMPTY);
	assert(get_u16(captured_output + 12) == GUI_PROTOCOL_WINDOW_CREATE);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM);
	server.state.surface_limit++;
	client_message(peer, &header, create, sizeof(create));
	ordinary = peer->windows;
	assert(ordinary != NULL);
	assert(ordinary->window_id != 0 && ordinary->reserved_bytes == 64 * 48 * 4);
	assert(server.state.surface_bytes == frame + ordinary->reserved_bytes);
	header.type = GUI_PROTOCOL_WINDOW_DESTROY;
	put_u32(create, ordinary->window_id);
	client_message(peer, &header, create, 4);
	assert(peer->windows == NULL && peer->surface_bytes == 0);
	header.type = GUI_PROTOCOL_WINDOW_CREATE;
	put_u32(create, 100);
	client_message(peer, &header, create, sizeof(create));
	ordinary = peer->windows;
	assert(ordinary != NULL);
	assert(ordinary->reserved_bytes == 64 * 48 * 4);
	fail_surface_allocation = 1;
	stage(ordinary, 1, 1);
	assert(ordinary->window_id != 0 && ordinary->configure_serial == 2);
	assert(ordinary->committed == NULL && ordinary->staging == NULL);
	assert(ordinary->reserved_bytes == 0 && server.state.surface_bytes == frame);
	assert(ordinary->width == 64 && ordinary->height == 48 && ordinary->client->hello == 1);
	stage(ordinary, 1, 2);
	assert(ordinary->staging == NULL && server.state.surface_bytes == frame);
	stage(ordinary, 2, 2);
	assert(ordinary->staging != NULL && server.state.surface_bytes == frame + 64 * 48 * 4);
	commit_staged(ordinary);
	assert(ordinary->committed != NULL && ordinary->reserved_bytes == 0);
	cleanup_server(&server);
}

static void test_transitions(void)
{
	static wm_server server;
	wm_window *client;
	gui_surface *old;
	size_t old_bytes = 64 * 48 * 4;
	size_t next_bytes = 96 * 64 * 4;

	init_server(&server, 256, 192);
	client = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	old = gui_surface_create(64, 48);
	assert(old != NULL);
	gui_fill_rect(old, (gui_rect) { 0, 0, 64, 48 }, 0x00123456);
	client->committed = old;
	client->committed_bytes = old_bytes;
	server.state.surface_bytes = old_bytes;
	client->client->surface_bytes = old_bytes;
	server.state.surface_limit = server.state.frame_bytes + old_bytes + next_bytes - 1;
	assert(wm_resize_window(&server, client, 32, 48, 96, 64) == -ENOMEM);
	assert(client->configure_serial == 1 && client->x == 64 && client->y == 64);
	assert(client->width == 64 && client->height == 48 && client->committed == old);
	assert(!gui_protocol_output_pending(client->client->output));
	server.state.surface_limit++;
	assert(wm_resize_window(&server, client, 32, 48, 96, 64) == 1);
	assert(client->configure_serial == 2 && client->reserved_bytes == next_bytes);
	assert(client->transition_pending && server.state.surface_bytes == old_bytes + next_bytes);
	stage(client, 1, 2);
	assert(client->staging == NULL && client->reserved_bytes == next_bytes);
	stage(client, 2, 2);
	assert(client->staging != NULL && client->reserved_bytes == 0);
	assert(server.state.surface_bytes == old_bytes + next_bytes);
	assert(wm_resize_window(&server, client, 16, 40, 80, 56) == 1);
	assert(client->configure_serial == 3 && client->staging == NULL);
	fail_surface_allocation = 1;
	stage(client, 3, 3);
	assert(client->configure_serial == 4 && client->width == 64 && client->height == 48);
	assert(client->x == 64 && client->y == 64 && client->committed == old);
	assert(client->staging == NULL && client->reserved_bytes == 0);
	assert(!client->transition_pending && server.state.surface_bytes == old_bytes);
	assert(old->pixels[0] == 0x00123456 && client->client->hello == 1);
	drain_output(client);
	assert(wm_resize_window(&server, client, 32, 48, 96, 64) == 1);
	stage(client, 5, 4);
	server.state.composite = gui_surface_create(256, 192);
	assert(server.state.composite != NULL);
	server.state.framebuffer = 123;
	server.state.scanout_stride = 256 * 4;
	server.state.graphics = 1;
	scanout_result = GUI_FLUSH_ERROR;
	commit_staged(client);
	assert(client->configure_serial == 6 && client->committed == old);
	assert(client->width == 64 && client->height == 48 && client->x == 64 && client->y == 64);
	assert(client->reserved_bytes == 0 && client->staging == NULL && client->client->hello == 1);
	assert(server.state.surface_bytes == old_bytes && old->pixels[0] == 0x00123456);
	assert(wm_resize_window(&server, client, 32, 48, 96, 64) == 0);
	assert(client->configure_serial == 8 && client->width == 64 && client->reserved_bytes == 0);
	server.state.graphics = 0;
	stage(client, 8, 5);
	assert(client->staging != NULL);
	client->configure_serial = UINT64_MAX - 1;
	assert(wm_resize_window(&server, client, 32, 48, 96, 64) == -EOVERFLOW);
	assert(client->staging != NULL && client->width == 64);
	cleanup_server(&server);
}

static void test_surface_flow(void)
{
	app_gui_surface_flow flow = { 0 };
	uint8_t error[sizeof(gui_protocol_wire_error)] = { 0 };
	uint8_t reply[sizeof(gui_protocol_wire_surface_data_reply)] = { 0 };

	put_u32(error, (uint32_t)-ENOMEM);
	put_u16(error + 4, GUI_PROTOCOL_SURFACE_BEGIN);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_ERROR, error, sizeof(error)) == 1);
	assert(flow.retry_pending);
	flow.retry_at_ms = UINT64_MAX;
	app_gui_surface_defer(&flow);
	assert(flow.retry_at_ms == UINT64_MAX && !app_gui_surface_retry_ready(&flow));
	app_gui_surface_discard_reply(&flow, 2, 9, 0, 1);
	flow.retry_at_ms = 0;
	assert(!app_gui_surface_retry_ready(&flow));
	put_u32(error, (uint32_t)-EINVAL);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_ERROR, error, sizeof(error)) == 1);
	assert(flow.discard_reply);
	put_u16(error + 4, GUI_PROTOCOL_SURFACE_DATA);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_ERROR, error, sizeof(error)) == 1);
	assert(app_gui_surface_retry_ready(&flow) && !flow.retry_pending);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_ERROR, error, sizeof(error)) == -1);
	app_gui_surface_defer(&flow);
	flow.retry_at_ms = 0;
	app_gui_surface_discard_reply(&flow, 2, 9, 0, 1);
	put_u32(reply, 8);
	put_u64(reply + 4, 2);
	put_u64(reply + 12, 9);
	put_u32(reply + 24, 1);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_SURFACE_DATA_REPLY,
		reply, sizeof(reply)) == -1);
	put_u32(reply, 7);
	put_u32(reply + 24, 2);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_SURFACE_DATA_REPLY,
		reply, sizeof(reply)) == -1);
	put_u32(reply + 24, 1);
	assert(app_gui_surface_flow_message(&flow, 7, GUI_PROTOCOL_SURFACE_DATA_REPLY,
		reply, sizeof(reply)) == 1);
	assert(app_gui_surface_retry_ready(&flow));
}

static void drain_peer(wm_client *peer)
{
	captured_bytes = 0;
	assert(gui_protocol_output_flush(peer->output, capture_output, NULL) == GUI_PROTOCOL_OUTPUT_EMPTY);
}

static void create_record(wm_client *peer, uint32_t request_id, uint32_t width, uint32_t height)
{
	gui_protocol_header header = { .type = GUI_PROTOCOL_WINDOW_CREATE };
	uint8_t create[sizeof(gui_protocol_wire_window_create)] = { 0 };

	put_u32(create, request_id);
	put_u32(create + 4, width);
	put_u32(create + 8, height);
	client_message(peer, &header, create, sizeof(create));
}

static void destroy_record(wm_client *peer, uint32_t window_id)
{
	gui_protocol_header header = { .type = GUI_PROTOCOL_WINDOW_DESTROY };
	uint8_t destroy[sizeof(gui_protocol_wire_window_id)];

	put_u32(destroy, window_id);
	client_message(peer, &header, destroy, sizeof(destroy));
}

static void test_peer_surface_capacity(void)
{
	static wm_server server;
	wm_client *peer;
	wm_client *healthy;
	wm_window *first;
	uint32_t first_identity;
	uint32_t saved_identity;
	size_t frame;

	init_server(&server, 256, 192);
	frame = server.state.frame_bytes;
	peer = wm_add_client(&server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
	assert(peer != NULL);
	peer->hello = 1;
	for (unsigned int index = 0; index < 8; index++) {
		create_record(peer, index + 1, 256, 192);
		assert(peer->windows != NULL && peer->hello == 1);
		drain_peer(peer);
	}
	assert(peer->surface_bytes == app_gui_peer_surface_budget(frame));
	first = peer->windows;
	first_identity = first->window_id;
	stage(first, 1, 1);
	commit_staged(first);
	assert(first->committed != NULL && peer->surface_bytes == 8 * frame);
	stage(first, 1, 2);
	drain_peer(peer);
	assert(first->staging == NULL && first->committed != NULL && peer->hello == 1);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM);
	saved_identity = server.next_window_id;
	create_record(peer, 9, 1, 1);
	drain_peer(peer);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM);
	assert(server.next_window_id == saved_identity && peer->surface_bytes == 8 * frame);
	healthy = wm_add_client(&server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
	assert(healthy != NULL);
	healthy->hello = 1;
	create_record(healthy, 10, 256, 192);
	assert(healthy->windows != NULL && healthy->surface_bytes == frame);
	drain_peer(healthy);
	destroy_record(peer, first_identity);
	drain_peer(peer);
	assert(peer->surface_bytes == 7 * frame && healthy->surface_bytes == frame);
	create_record(peer, 11, 256, 192);
	assert(peer->windows->window_id > saved_identity && peer->surface_bytes == 8 * frame);
	assert(server.state.surface_bytes == 9 * frame);
	drain_peer(peer);
	cleanup_server(&server);
}

static void test_record_capacity(void)
{
	static wm_server server;
	wm_client *peer;
	wm_client *connections[40];
	wm_window *first;
	wm_window *last;
	wm_window *shell;
	uint32_t identities[40];
	uint32_t saved_id;
	size_t saved_peer;
	size_t saved_server;
	int descriptor;

	assert(app_gui_peer_surface_budget(0) == 0);
	assert(app_gui_peer_surface_budget(1) == 8);
	assert(app_gui_peer_surface_budget(SIZE_MAX) == APP_GUI_PEER_SURFACE_BYTES);
	init_server(&server, 1024, 768);
	peer = wm_add_client(&server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
	assert(peer != NULL);
	peer->hello = 1;
	for (unsigned int index = 0; index < 40; index++) {
		create_record(peer, index + 1, 1, 1);
		assert(peer->windows != NULL && peer->hello == 1);
		identities[index] = peer->windows->window_id;
		assert(index == 0 || identities[index] > identities[index - 1]);
		drain_peer(peer);
	}
	first = wm_find_window(&server, identities[0]);
	last = wm_find_window(&server, identities[39]);
	assert(first != NULL && last != NULL && server.client_count == 1);
	stage(first, 1, 1);
	stage(last, 1, 1);
	commit_staged(last);
	assert(first->staging != NULL && last->committed != NULL && peer->surface_bytes == 40 * 4);
	destroy_record(peer, identities[10]);
	assert(wm_find_window(&server, identities[10]) == NULL && first->staging != NULL);
	assert(last->committed != NULL && peer->surface_bytes == 39 * 4);
	drain_peer(peer);
	destroy_record(peer, identities[10]);
	drain_peer(peer);
	assert(get_u16(captured_output + 12) == GUI_PROTOCOL_WINDOW_DESTROY);
	assert((int32_t)get_u32(captured_output + 8) == -EINVAL && peer->hello == 1);
	saved_peer = peer->record_bytes;
	saved_server = server.record_bytes;
	saved_id = server.next_window_id;
	peer->record_bytes = APP_GUI_PEER_RECORD_BYTES - sizeof(wm_window) + 1;
	create_record(peer, 100, 1, 1);
	drain_peer(peer);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM && server.next_window_id == saved_id);
	peer->record_bytes--;
	create_record(peer, 101, 1, 1);
	assert(peer->record_bytes == APP_GUI_PEER_RECORD_BYTES && server.next_window_id > saved_id);
	drain_peer(peer);
	create_record(peer, 102, 1, 1);
	drain_peer(peer);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM && peer->hello == 1);
	peer->record_bytes = saved_peer + sizeof(wm_window);
	destroy_record(peer, server.next_window_id);
	drain_peer(peer);
	assert(peer->record_bytes == saved_peer && server.record_bytes == saved_server);
	saved_id = server.next_window_id;
	server.record_bytes = APP_GUI_WM_RECORD_BYTES - sizeof(wm_window) + 1;
	create_record(peer, 103, 1, 1);
	drain_peer(peer);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM && server.next_window_id == saved_id);
	server.record_bytes--;
	create_record(peer, 104, 1, 1);
	assert(server.record_bytes == APP_GUI_WM_RECORD_BYTES);
	drain_peer(peer);
	create_record(peer, 105, 1, 1);
	drain_peer(peer);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM && peer->hello == 1);
	server.record_bytes = saved_server + sizeof(wm_window);
	destroy_record(peer, server.next_window_id);
	drain_peer(peer);
	assert(peer->record_bytes == saved_peer && server.record_bytes == saved_server);
	saved_id = server.next_window_id;
	fail_record_allocation = 1;
	create_record(peer, 106, 1, 1);
	drain_peer(peer);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM);
	assert(server.next_window_id == saved_id && first->staging != NULL && last->committed != NULL);
	assert(peer->record_bytes == saved_peer && server.record_bytes == saved_server);
	descriptor = dup(STDOUT_FILENO);
	assert(descriptor >= 0);
	server.record_bytes = APP_GUI_WM_RECORD_BYTES;
	assert(wm_add_client(&server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY) == NULL);
	assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
	server.record_bytes = saved_server;
	descriptor = dup(STDOUT_FILENO);
	assert(descriptor >= 0);
	server.record_bytes = APP_GUI_WM_RECORD_BYTES - sizeof(wm_client);
	assert(wm_add_client(&server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY) == NULL);
	assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
	assert(server.record_bytes == APP_GUI_WM_RECORD_BYTES - sizeof(wm_client));
	assert(server.client_count == 1 && peer->record_bytes == saved_peer);
	server.record_bytes = saved_server;
	descriptor = dup(STDOUT_FILENO);
	assert(descriptor >= 0);
	fail_record_allocation = 1;
	assert(wm_add_client(&server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY) == NULL);
	assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
	assert(server.record_bytes == saved_server && peer->hello == 1);
	for (unsigned int index = 0; index < 40; index++) {
		connections[index] = wm_add_client(&server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
		assert(connections[index] != NULL);
		connections[index]->hello = 1;
		create_record(connections[index], 200 + index, 1, 1);
		assert(connections[index]->windows != NULL && connections[index]->hello == 1);
		stage(connections[index]->windows, 1, 1);
		commit_staged(connections[index]->windows);
		drain_peer(connections[index]);
	}
	assert(server.client_count == 41 && peer->surface_bytes == 39 * 4);
	shell = init_client(&server, server.next_window_id, GUI_PROTOCOL_ENDPOINT_SHELL);
	wm_send_shell_snapshot(&server, shell->client);
	unsigned int snapshots = 0;
	uint32_t previous = 0;

	while (shell->client->snapshot_pending || gui_protocol_output_pending(shell->client->output)) {
		uint32_t cursor = shell->client->snapshot_cursor;
		unsigned int batch = 0;

		wm_pump_shell_snapshot(&server, shell->client);
		assert(cursor == shell->client->snapshot_cursor);
		drain_peer(shell->client);
		for (size_t offset = 0; offset < captured_bytes;) {
			assert(get_u16(captured_output + offset + 4) == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT);
			uint32_t identity = get_u32(captured_output + offset + GUI_PROTOCOL_HEADER_SIZE);

			assert(identity > previous);
			previous = identity;
			batch++;
			offset += get_u32(captured_output + offset);
		}
		assert(batch <= WM_SHELL_SNAPSHOT_QUANTUM);
		snapshots += batch;
		wm_pump_shell_snapshot(&server, shell->client);
	}
	assert(snapshots == 79 && shell->client->hello == 1);
	for (unsigned int index = 0; index < 40; index++) {
		descriptor = connections[index]->fd;
		wm_disconnect_client(&server, connections[index]);
		assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
	}
	assert(server.client_count == 2 && first->staging != NULL && last->committed != NULL);
	assert(wm_poll_resize(&server, server.client_count) == 0);
	cleanup_server(&server);
}

static void test_shell_keyboard_input(void)
{
	desktop_state model = { .window_id = 1 };
	gui_protocol_header header = { .type = GUI_PROTOCOL_INPUT_EVENT };
	uint8_t payload[sizeof(gui_protocol_wire_input_event)] = { 0 };

	put32(payload, model.window_id);
	put32(payload + 4, GUI_INPUT_EVENT_KEY);
	put32(payload + 8, 'a');
	assert(desktop_handle_message(&model, &header, payload, sizeof(payload)) == 0);
	assert(model.redraw == 0 && model.mouse_buttons == 0);
	put32(payload, model.window_id + 1);
	assert(desktop_handle_message(&model, &header, payload, sizeof(payload)) < 0);
}

static int block_shell_output;

static ssize_t capture_shell_output(void *context, const void *bytes, size_t count)
{
	if (block_shell_output && captured_bytes >= 32) {
		errno = EAGAIN;
		return -1;
	}
	if (block_shell_output && count > 32 - captured_bytes)
		count = 32 - captured_bytes;
	return capture_output(context, bytes, count);
}

static size_t feed_shell_model(desktop_state *model, uint32_t future_id,
		unsigned int *snapshots, int *future_seen, int *changed_seen)
{
	size_t messages = 0;

	for (size_t offset = 0; offset < captured_bytes;) {
		gui_protocol_header header;
		const uint8_t *payload = captured_output + offset + GUI_PROTOCOL_HEADER_SIZE;

		assert(gui_protocol_decode_header(captured_output + offset,
			captured_bytes - offset, &header) == 0);
		assert(header.length <= captured_bytes - offset);
		if (header.type == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT) {
			assert(get_u32(payload) != future_id);
			(*snapshots)++;
		}
		if (header.type == GUI_PROTOCOL_WINDOW_LIST_ADD && get_u32(payload) == future_id)
			*future_seen = 1;
		if (header.type == GUI_PROTOCOL_WINDOW_LIST_CHANGE && get_u32(payload) == 1) {
			assert(get_u32(payload + 4) == 77);
			*changed_seen = 1;
		}
		assert(desktop_handle_window_list(model, header.type, payload,
			header.length - GUI_PROTOCOL_HEADER_SIZE) == 0);
		if (header.type == GUI_PROTOCOL_WINDOW_LIST_CHANGE && get_u32(payload) == 1) {
			model->redraw = 0;
			assert(desktop_handle_window_list(model, header.type, payload,
				header.length - GUI_PROTOCOL_HEADER_SIZE) == 0);
			assert(model->redraw == 0);
		}
		offset += header.length;
		messages++;
	}
	assert(messages <= WM_SHELL_SNAPSHOT_QUANTUM);
	return messages;
}

static void check_model_pages(desktop_state *model)
{
	const desktop_window *expected = model->windows;
	size_t reached = 0;

	assert(desktop_prepare_taskbar(model) == 0);
	size_t pages = model->painted_taskbar.pages;

	for (size_t page = 0; page < pages; page++) {
		model->taskbar_page = page;
		assert(desktop_prepare_taskbar(model) == 0);
		desktop_commit_taskbar(model);
		for (size_t slot = 0; slot < model->displayed_taskbar.count; slot++) {
			uint32_t pointer_x = model->displayed_taskbar.first_x +
				slot * DESKTOP_TASKBAR_BUTTON_STRIDE + 10;
			const desktop_window *hit = desktop_taskbar_window(model, pointer_x, 750);

			assert(expected != NULL && hit == expected);
			assert(hit->window_id == model->displayed_taskbar.buttons[slot].window_id);
			assert(pointer_x + DESKTOP_TASKBAR_BUTTON_WIDTH - 10 < model->width - 40);
			expected = expected->next;
			reached++;
		}
	}
	assert(expected == NULL && reached == model->window_count);
	assert(desktop_taskbar_control(model, &model->displayed_taskbar, 74, 750) == -1);
	assert(desktop_taskbar_control(model, &model->displayed_taskbar, 970, 750) == 1);
}

static void test_large_shell_stream(void)
{
	static wm_server server;
	desktop_state model = { .width = 1024, .height = 768 };
	wm_client *peer;
	wm_window *shell;
	wm_window *first;
	uint32_t future_id;
	unsigned int snapshots = 0;
	unsigned int turns = 0;
	int future_seen = 0;
	int changed_seen = 0;
	size_t wire_bytes = 0;

	init_server(&server, 256, 192);
	peer = wm_add_client(&server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
	assert(peer != NULL);
	peer->hello = 1;
	for (unsigned int index = 0; index < 256; index++) {
		create_record(peer, index + 1, 1, 1);
		peer->windows->title_bytes = GUI_PROTOCOL_MAX_TITLE_BYTES;
		memset(peer->windows->title, 's', GUI_PROTOCOL_MAX_TITLE_BYTES);
		drain_peer(peer);
	}
	shell = init_client(&server, server.next_window_id, GUI_PROTOCOL_ENDPOINT_SHELL);
	wm_send_shell_snapshot(&server, shell->client);
	assert(shell->client->snapshot_cursor == 8);
	first = wm_find_window(&server, 1);
	first->x = 77;
	for (unsigned int repeat = 0; repeat < 256; repeat++)
		wm_notify_shell_window(&server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, first);
	destroy_record(peer, 2);
	destroy_record(peer, 100);
	create_record(peer, 500, 1, 1);
	future_id = peer->windows->window_id;
	assert(future_id > shell->client->snapshot_end && server.list_pending == 3);
	captured_bytes = 0;
	block_shell_output = 1;
	assert(gui_protocol_output_flush(shell->client->output, capture_shell_output, NULL) ==
		GUI_PROTOCOL_OUTPUT_BLOCKED);
	assert(captured_bytes == 32);
	wm_pump_shell_snapshot(&server, shell->client);
	assert(shell->client->snapshot_cursor == 8 && server.removed_windows != NULL);
	block_shell_output = 0;
	assert(gui_protocol_output_flush(shell->client->output, capture_shell_output, NULL) ==
		GUI_PROTOCOL_OUTPUT_EMPTY);
	wire_bytes += captured_bytes;
	feed_shell_model(&model, future_id, &snapshots, &future_seen, &changed_seen);
	while (shell->client->snapshot_pending || server.list_pending != 0) {
		assert(turns++ < 1000);
		if (!future_seen)
			wm_notify_shell_window(&server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, first);
		wm_pump_shell_snapshot(&server, shell->client);
		drain_peer(shell->client);
		wire_bytes += captured_bytes;
		feed_shell_model(&model, future_id, &snapshots, &future_seen, &changed_seen);
	}
	assert(snapshots == 255 && future_seen && changed_seen && wire_bytes > GUI_PROTOCOL_OUTPUT_LIMIT);
	assert(model.window_count == 255 && shell->client->hello == 1);
	assert(desktop_window_by_id(&model, 2) == NULL && desktop_window_by_id(&model, 100) == NULL);
	assert(server.removed_windows == NULL && server.list_pending == 0);
	check_model_pages(&model);
	wm_set_input_owner(&server, wm_find_window(&server, 10));
	wm_pump_shell_snapshot(&server, shell->client);
	drain_peer(shell->client);
	feed_shell_model(&model, future_id, &snapshots, &future_seen, &changed_seen);
	assert(model.taskbar_page == 1);
	assert(desktop_prepare_taskbar(&model) == 0);
	desktop_commit_taskbar(&model);
	assert(desktop_taskbar_window(&model, 98, 750)->window_id == 10);
	destroy_record(peer, 10);
	wm_pump_shell_snapshot(&server, shell->client);
	drain_peer(shell->client);
	feed_shell_model(&model, future_id, &snapshots, &future_seen, &changed_seen);
	assert(desktop_taskbar_window(&model, 98, 750) == NULL);
	create_record(peer, 501, 1, 1);
	assert(peer->windows->window_id > future_id);
	wm_pump_shell_snapshot(&server, shell->client);
	drain_peer(shell->client);
	feed_shell_model(&model, future_id, &snapshots, &future_seen, &changed_seen);
	assert(desktop_taskbar_window(&model, 98, 750) == NULL);
	check_model_pages(&model);
	while (model.windows != NULL) {
		uint8_t remove[sizeof(gui_protocol_wire_window_list)] = { 0 };

		put32(remove, model.windows->window_id);
		assert(desktop_handle_window_list(&model, GUI_PROTOCOL_WINDOW_LIST_REMOVE,
			remove, sizeof(remove)) == 0);
	}
	model.record_bytes -= 2 * desktop_taskbar_capacity(&model, 0) * sizeof(desktop_button);
	free(model.taskbar_storage);
	assert(model.record_bytes == 0 && model.window_count == 0);
	cleanup_server(&server);
}

static unsigned int rejected_begins;
static unsigned int completed_frames;
static unsigned int rollback_checks;
static int fail_commit;

static void terminal_message(void *context, const gui_protocol_header *header,
				const void *payload, size_t payload_bytes)
{
	wm_client *peer = context;
	wm_window *client = peer->windows;
	gui_surface *previous = client == NULL ? NULL : client->committed;
	int allocation_failure = fail_surface_allocation;
	int surface_begin = header->type == GUI_PROTOCOL_SURFACE_BEGIN || header->type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN;
	int flush_failure = fail_commit && header->type == GUI_PROTOCOL_SURFACE_COMMIT &&
		client != NULL && client->staging != NULL;

	if (flush_failure) {
		peer->server->state.graphics = 1;
		scanout_result = GUI_FLUSH_ERROR;
		fail_commit = 0;
	}
	client_message(context, header, payload, payload_bytes);
	client = peer->windows;
	if (flush_failure)
		peer->server->state.graphics = 0;
	if (surface_begin && client != NULL && client->staging == NULL)
		rejected_begins++;
	if (header->type == GUI_PROTOCOL_SURFACE_COMMIT && client != NULL &&
	    client->committed != previous && client->committed != NULL)
		completed_frames++;
	if ((allocation_failure && surface_begin) || flush_failure) {
		assert(client != NULL && client->committed == previous && client->staging == NULL);
		assert(client->reserved_bytes == 0 && client->width == 320 && client->height == 192);
		assert(peer->hello == 1 && !client->transition_pending);
		rollback_checks++;
	}
}

static void pump_terminal(wm_client *client)
{
	struct pollfd descriptor = { .fd = client->fd, .events = POLLIN };
	uint8_t input[4096];
	int messages;
	int ready = poll(&descriptor, 1, client->drain_input ? 0 : 100);

	assert(ready >= 0);
	if (descriptor.revents & POLLIN) {
		ssize_t count = read(client->fd, input, sizeof(input));

		if (count == 0)
			return;
		assert(count > 0);
		assert(gui_protocol_input_append(client->input, input, (size_t)count) == 0);
	}
	messages = gui_protocol_input_drain(client->input, terminal_message, client);
	assert(messages >= 0 && client->hello >= 0);
	client->drain_input = messages == 8 && gui_protocol_input_pending(client->input);
	assert(client_flush(client) == 0 || client->windows == NULL);
}

static void wait_counter(wm_client *client, const unsigned int *counter, unsigned int target)
{
	struct timespec start;
	struct timespec now;

	assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
	while (*counter < target) {
		pump_terminal(client);
		assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
		assert(now.tv_sec - start.tv_sec < 20);
	}
}

static void touch_marker(const char *path)
{
	int descriptor = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);

	assert(descriptor >= 0 && close(descriptor) == 0);
}

static void test_terminal_recovery(void)
{
	static wm_server server;
	wm_window *client;
	wm_window *pressure;
	wm_client *peer;
	int listener;
	int descriptor;
	int status;
	pid_t child;
	size_t terminal_bytes = 320 * 192 * 4;
	struct pollfd pending;

	init_server(&server, 1024, 768);
	server.state.surface_limit = 2 * server.state.frame_bytes + 2 * terminal_bytes;
	server.state.composite = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL);
	server.state.scanout_stride = 1024 * 4;
	server.state.framebuffer = 123;
	unlink(WM_SOCKET_PATH);
	unlink("/tmp/gui-wm-terminal.paint");
	unlink("/tmp/gui-wm-terminal.finish");
	listener = bind_listener();
	assert(listener >= 0);
	child = fork();
	assert(child >= 0);
	if (child == 0) {
		execl(WM_TERMINAL_PATH, WM_TERMINAL_PATH, "--command",
			"printf 'surface budget initial\\n'; "
			"while test ! -e /tmp/gui-wm-terminal.paint; do sleep 1; done; "
			"printf 'surface budget repaint\\n'; "
			"while test ! -e /tmp/gui-wm-terminal.finish; do sleep 1; done", NULL);
		_exit(127);
	}
	pending = (struct pollfd) { .fd = listener, .events = POLLIN };
	assert(poll(&pending, 1, 10000) == 1);
	descriptor = accept(listener, NULL, NULL);
	assert(descriptor >= 0);
	assert(fcntl(descriptor, F_SETFL, O_NONBLOCK) == 0);
	peer = wm_add_client(&server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	assert(peer != NULL);
	wait_counter(peer, &completed_frames, 1);
	client = peer->windows;
	assert(client != NULL);
	pressure = init_client(&server, 1, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	pressure->committed = gui_surface_create(1024, 768);
	assert(pressure->committed != NULL);
	pressure->committed_bytes = server.state.frame_bytes;
	pressure->width = 1024;
	pressure->height = 768;
	server.state.surface_bytes += pressure->committed_bytes;
	pressure->client->surface_bytes += pressure->committed_bytes;
	assert(wm_resize_window(&server, pressure, 64, 64, 320, 192) == 1);
	touch_marker("/tmp/gui-wm-terminal.paint");
	wait_counter(peer, &rejected_begins, 1);
	assert(kill(child, 0) == 0 && client->committed != NULL);
	assert(wm_resize_window(&server, client, 64, 64, 400, 256) == -ENOMEM);
	window_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, -ENOMEM);
	assert(client_flush(client->client) == 0);
	client_discard_reservation(pressure);
	wait_counter(peer, &completed_frames, completed_frames + 1);
	assert(client->width == 320 && client->height == 192 && kill(child, 0) == 0);
	assert(wm_resize_window(&server, client, 64, 64, 256, 192) == 1);
	fail_surface_allocation = 1;
	assert(client_flush(client->client) == 0);
	wait_counter(peer, &rollback_checks, 1);
	wait_counter(peer, &completed_frames, completed_frames + 1);
	assert(client->configure_serial == 3 && kill(child, 0) == 0);
	assert(wm_resize_window(&server, client, 64, 64, 256, 192) == 1);
	fail_commit = 1;
	assert(client_flush(client->client) == 0);
	wait_counter(peer, &rollback_checks, 2);
	wait_counter(peer, &completed_frames, completed_frames + 1);
	assert(client->configure_serial == 5 && kill(child, 0) == 0);
	assert(server.state.surface_bytes == server.state.frame_bytes + terminal_bytes);
	touch_marker("/tmp/gui-wm-terminal.finish");
	for (unsigned int attempt = 0;; attempt++) {
		pid_t result = waitpid(child, &status, WNOHANG);

		assert(result >= 0 && attempt < 2000);
		if (result == child)
			break;
		pump_terminal(peer);
		if (usleep(10000) < 0)
			assert(errno == EINTR);
	}
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	close(listener);
	unlink(WM_SOCKET_PATH);
	unlink("/tmp/gui-wm-terminal.paint");
	unlink("/tmp/gui-wm-terminal.finish");
	cleanup_server(&server);
}

static uint64_t begin_drawing_measurement(const wm_server *server)
{
	memset(&measured_work, 0, sizeof(measured_work));
	measure_drawing = 1;
	scanout_result = GUI_FLUSH_COMPLETE;
	return server->state.frame_number;
}

static void test_drawing_counter_clipping(void)
{
	gui_surface *destination = gui_surface_create(8, 6);
	gui_surface *source = gui_surface_create(4, 4);

	assert(destination != NULL && source != NULL);
	memset(&measured_work, 0, sizeof(measured_work));
	measure_drawing = 1;
	measured_fill(destination, (gui_rect) { -3, -2, 5, 4 }, 0);
	measured_fill(destination, (gui_rect) { 8, 6, 2, 2 }, 0);
	measured_blit(destination, -1, 1, source, &(gui_rect) { -2, -1, 8, 7 });
	measured_blit(destination, -2, -2, source, &(gui_rect) { 3, 3, 4, 4 });
	measured_blit(destination, 0, 0, source, NULL);
	measure_drawing = 0;
	assert(measured_work.filled_pixels == 4 && measured_work.blitted_pixels == 16);
	gui_surface_destroy(destination);
	gui_surface_destroy(source);
}

static void report_drawing_work(const char *name, const wm_server *server,
			       uint64_t previous_frame, int presented)
{
	uint64_t frames = server->state.frame_number - previous_frame;

	measure_drawing = 0;
	assert(frames == (uint64_t)presented);
	if (!presented) {
		assert(measured_work.flush_requests == 0 && measured_work.writer_calls == 0 && measured_work.written_bytes == 0);
		assert(measured_work.filled_pixels == 0 && measured_work.blitted_pixels == 0);
	} else if (!strcmp(name, "cursor")) {
		assert(measured_work.filled_pixels == 512 && measured_work.blitted_pixels == 512);
		assert(measured_work.flush_requests == 2 && measured_work.writer_calls == 32 && measured_work.written_bytes == 2048);
	} else if (!strcmp(name, "small-window-move")) {
		assert(measured_work.filled_pixels == 219072 && measured_work.blitted_pixels == 136704);
		assert(measured_work.flush_requests == 1 && measured_work.writer_calls == 224 && measured_work.written_bytes == 301056);
	} else if (!strcmp(name, "visible-commit")) {
		assert(measured_work.filled_pixels == 184320 && measured_work.blitted_pixels == 122880);
		assert(measured_work.flush_requests == 1 && measured_work.writer_calls == 192 && measured_work.written_bytes == 245760);
	} else {
		assert(measured_work.filled_pixels == 1572864 && measured_work.blitted_pixels == 786432);
		assert(measured_work.flush_requests == 1 && measured_work.writer_calls == 1 && measured_work.written_bytes == 3145728);
	}
	printf("gui-drawing-work: {\"case\":\"%s\",\"filled_pixels\":%llu,"
		"\"blitted_pixels\":%llu,\"flush_requests\":%llu,\"writer_calls\":%llu,"
		"\"written_bytes\":%llu,\"presentations\":%llu}\n", name,
		(unsigned long long)measured_work.filled_pixels,
		(unsigned long long)measured_work.blitted_pixels,
		(unsigned long long)measured_work.flush_requests,
		(unsigned long long)measured_work.writer_calls,
		(unsigned long long)measured_work.written_bytes,
		(unsigned long long)frames);
}

static void assert_full_scene(wm_server *server)
{
	gui_surface *actual = server->state.composite;
	gui_surface *oracle = gui_surface_create(server->state.width, server->state.height);

	assert(oracle != NULL);
	server->state.composite = oracle;
	wm_paint_rect(server, (gui_rect) { 0, 0, server->state.width, server->state.height });
	server->state.composite = actual;
	assert(memcmp(actual->pixels, oracle->pixels, (size_t)actual->stride * actual->height * 4) == 0);
	gui_surface_destroy(oracle);
}

static void emit_pointer(wm_server *server, uint16_t cursor_x, uint16_t cursor_y,
			 uint8_t buttons)
{
	gui_input_event pointer = { .type = GUI_INPUT_EVENT_MOUSE,
		.mouse = { .x = (uint32_t)cursor_x * PSAUX_ABSOLUTE_MAX / server->state.width,
			.y = (uint32_t)cursor_y * PSAUX_ABSOLUTE_MAX / server->state.height,
			.buttons = buttons, .absolute = 1 } };

	wm_emit_input(server, &pointer);
}

static wm_window *init_title_click_test(wm_server *server)
{
	wm_window *target;

	init_server(server, 320, 240);
	server->state.graphics = 1;
	server->state.framebuffer = 123;
	server->state.scanout_stride = 320 * 4;
	server->state.composite = gui_surface_create(320, 240);
	assert(server->state.composite != NULL);
	server->work_area_valid = 1;
	server->work_width = 320;
	server->work_height = 216;
	target = init_client(server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	target->x = 64;
	target->y = 100;
	target->width = 160;
	target->height = 100;
	scanout_result = GUI_FLUSH_COMPLETE;
	fullscreen_stage(target, 0x00225688);
	commit_staged(target);
	test_clock_ms = 1000;
	return target;
}

static void test_title_double_click(void)
{
	static wm_server server;
	enum {
		EXPIRED, POINTER_EXCURSION, TITLE_DRAG, RIGHT_CLICK, MIDDLE_CLICK,
		MOUSE_RESET, KEY_RESET, KEY_PRESS, CONTENT_CLICK, BORDER_CLICK,
		OTHER_WINDOW, DESTROYED_WINDOW, LONG_PRESS, MINIMIZED_WINDOW,
		FOCUS_TRANSFER, TITLE_CLICK_CASE_COUNT,
	};
	wm_window *target;

	use_test_clock = 1;
	target = init_title_click_test(&server);
	emit_pointer(&server, 80, 88, 1);
	assert(server.capture_client == target && !target->state_pending);
	emit_pointer(&server, 80, 88, 1);
	assert(server.capture_client == target && !target->state_pending);
	emit_pointer(&server, 80, 88, 0);
	assert(server.capture_client == NULL && server.title_click_window_id == target->window_id);
	assert(target->x == 64 && target->y == 100 && !target->maximized);
	test_clock_ms += WM_TITLE_DOUBLE_CLICK_MS;
	emit_pointer(&server, 84, 92, 1);
	assert(server.capture_client == NULL && server.title_click_window_id == 0);
	assert(target->state_pending && target->pending_state == GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED);
	assert(target->normal_x == 64 && target->normal_y == 100);
	assert(target->normal_width == 160 && target->normal_height == 100);
	assert(target->x == 4 && target->y == 28 && target->width == 312 && target->height == 184);
	fullscreen_stage(target, 0x00225688);
	commit_staged(target);
	assert(target->maximized && !target->state_pending);
	assert_full_scene(&server);
	emit_pointer(&server, 84, 92, 0);
	test_clock_ms += 100;
	emit_pointer(&server, 80, 16, 1);
	emit_pointer(&server, 80, 16, 0);
	test_clock_ms += 100;
	emit_pointer(&server, 80, 16, 1);
	assert(server.capture_client == NULL && server.title_click_window_id == 0);
	assert(target->state_pending && target->pending_state == GUI_PROTOCOL_WINDOW_STATE_NORMAL);
	fullscreen_stage(target, 0x00225688);
	commit_staged(target);
	assert(!target->maximized && !target->state_pending);
	assert(target->x == 64 && target->y == 100 && target->width == 160 && target->height == 100);
	emit_pointer(&server, 80, 16, 0);
	assert_full_scene(&server);
	cleanup_server(&server);

	for (unsigned int scenario = 0; scenario < TITLE_CLICK_CASE_COUNT; scenario++) {
		target = init_title_click_test(&server);
		emit_pointer(&server, 80, 88, 1);
		if (scenario == TITLE_DRAG) {
			emit_pointer(&server, 100, 88, 1);
			assert(target->x == 84 && target->y == 100);
			emit_pointer(&server, 80, 88, 1);
			assert(target->x == 64);
		}
		if (scenario == LONG_PRESS)
			test_clock_ms += WM_TITLE_DOUBLE_CLICK_MS + 1;
		emit_pointer(&server, 80, 88, 0);
		test_clock_ms += 100;
		switch (scenario) {
		case EXPIRED:
			test_clock_ms = 1000 + WM_TITLE_DOUBLE_CLICK_MS + 1;
			break;
		case POINTER_EXCURSION:
			emit_pointer(&server, 85, 88, 0);
			emit_pointer(&server, 80, 88, 0);
			break;
		case RIGHT_CLICK:
		case MIDDLE_CLICK:
			emit_pointer(&server, 80, 88, scenario == RIGHT_CLICK ? 4 : 2);
			emit_pointer(&server, 80, 88, 0);
			break;
		case MOUSE_RESET:
		case KEY_RESET:
		case KEY_PRESS: {
			gui_input_event reset = { .type = scenario == MOUSE_RESET ?
				GUI_INPUT_EVENT_MOUSE_RESET : scenario == KEY_RESET ?
				GUI_INPUT_EVENT_KEY_RESET : GUI_INPUT_EVENT_KEY,
				.key = { .scancode = 0x1e, .pressed = 1 } };

			wm_emit_input(&server, &reset);
			break;
		}
		case CONTENT_CLICK:
			emit_pointer(&server, 80, 108, 1);
			emit_pointer(&server, 80, 108, 0);
			break;
		case BORDER_CLICK:
			emit_pointer(&server, 62, 88, 1);
			emit_pointer(&server, 62, 88, 0);
			break;
		case OTHER_WINDOW: {
			wm_window *other = init_client(&server, server.next_window_id,
				GUI_PROTOCOL_ENDPOINT_ORDINARY);

			other->width = 160;
			other->height = 100;
			other->y = 100;
			other->stack_order = ++server.next_stack_order;
			break;
		}
		case DESTROYED_WINDOW: {
			wm_client *owner = target->client;

			assert(server.title_click_window_id == target->window_id);
			server.input_owner = NULL;
			window_destroy(target);
			client_destroy(owner);
			assert(server.title_click_window_id == 0);
			target = init_client(&server, server.next_window_id, GUI_PROTOCOL_ENDPOINT_ORDINARY);
			target->width = 160;
			target->height = 100;
			target->y = 100;
			break;
		}
		case MINIMIZED_WINDOW:
			assert(wm_request_minimize(&server, target) == 1);
			assert(server.title_click_window_id == 0);
			assert(wm_request_minimize(&server, target) == 1);
			break;
		case FOCUS_TRANSFER:
			wm_set_input_owner(&server, NULL);
			break;
		default:
			break;
		}
		emit_pointer(&server, 80, 88, 1);
		assert(!target->state_pending && !target->maximized);
		for (wm_window *window = server.windows; window != NULL; window = window->next)
			assert(!window->state_pending && !window->maximized);
		emit_pointer(&server, 80, 88, 0);
		cleanup_server(&server);
	}
	use_test_clock = 0;
	puts("gui-wm-busy: title double clicks toggle maximize without mistaking drags or unrelated input");
}

static void test_cursor_shapes(void)
{
	static wm_server server;
	const struct {
		uint32_t x;
		uint32_t y;
		wm_cursor_shape shape;
	} hover_cases[] = {
		{ 62, 140, WM_CURSOR_RESIZE_HORIZONTAL },
		{ 226, 140, WM_CURSOR_RESIZE_HORIZONTAL },
		{ 144, 74, WM_CURSOR_RESIZE_VERTICAL },
		{ 144, 202, WM_CURSOR_RESIZE_VERTICAL },
		{ 62, 74, WM_CURSOR_RESIZE_NW_SE },
		{ 226, 74, WM_CURSOR_RESIZE_NE_SW },
		{ 62, 202, WM_CURSOR_RESIZE_NE_SW },
		{ 226, 202, WM_CURSOR_RESIZE_NW_SE },
		{ 80, 88, WM_CURSOR_ARROW },
		{ 215, 88, WM_CURSOR_ARROW },
		{ 100, 140, WM_CURSOR_ARROW },
		{ 12, 12, WM_CURSOR_ARROW },
	};
	const struct {
		wm_capture_operation operation;
		wm_cursor_shape shape;
	} capture_cases[] = {
		{ WM_CAPTURE_MOVE, WM_CURSOR_MOVE },
		{ WM_CAPTURE_RESIZE_LEFT, WM_CURSOR_RESIZE_HORIZONTAL },
		{ WM_CAPTURE_RESIZE_RIGHT, WM_CURSOR_RESIZE_HORIZONTAL },
		{ WM_CAPTURE_RESIZE_TOP, WM_CURSOR_RESIZE_VERTICAL },
		{ WM_CAPTURE_RESIZE_BOTTOM, WM_CURSOR_RESIZE_VERTICAL },
		{ WM_CAPTURE_RESIZE_TOP_LEFT, WM_CURSOR_RESIZE_NW_SE },
		{ WM_CAPTURE_RESIZE_TOP_RIGHT, WM_CURSOR_RESIZE_NE_SW },
		{ WM_CAPTURE_RESIZE_BOTTOM_LEFT, WM_CURSOR_RESIZE_NE_SW },
		{ WM_CAPTURE_RESIZE_BOTTOM_RIGHT, WM_CURSOR_RESIZE_NW_SE },
	};
	const gui_input_event release = { .type = GUI_INPUT_EVENT_MOUSE };
	const gui_input_event mouse_reset = { .type = GUI_INPUT_EVENT_MOUSE_RESET };
	const gui_input_event key_reset = { .type = GUI_INPUT_EVENT_KEY_RESET };
	wm_window *target;
	uint64_t previous_frame;

	init_server(&server, 320, 240);
	server.state.graphics = 1;
	server.state.framebuffer = 123;
	server.state.scanout_stride = 320 * 4;
	server.state.composite = gui_surface_create(320, 240);
	assert(server.state.composite != NULL);
	target = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	target->x = 64;
	target->y = 100;
	target->width = 160;
	target->height = 100;
	scanout_result = GUI_FLUSH_COMPLETE;
	server.state.cursor_x = 100;
	server.state.cursor_y = 140;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(!server.state.cursor_loaded && server.state.cursor_shape == WM_CURSOR_ARROW);
	assert(wm_load_cursor(&server.state) == 0 && server.state.cursor_loaded);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.composite->pixels[140 * server.state.composite->stride + 100] ==
		WM_WINDOW_BORDER_COLOR);
	assert_full_scene(&server);
	for (size_t index = 0; index < sizeof(hover_cases) / sizeof(hover_cases[0]); index++) {
		server.state.cursor_x = hover_cases[index].x;
		server.state.cursor_y = hover_cases[index].y;
		assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
		assert(server.state.cursor_shape == hover_cases[index].shape);
		assert(server.state.painted_cursor_shape == hover_cases[index].shape);
		assert_full_scene(&server);
	}
	for (size_t index = 0; index < sizeof(capture_cases) / sizeof(capture_cases[0]); index++) {
		wm_begin_capture(&server, target, capture_cases[index].operation);
		server.state.mouse_buttons = 1;
		wm_present_input(&server, 0, 0);
		assert(server.state.cursor_shape == capture_cases[index].shape);
		assert_full_scene(&server);
		previous_frame = server.state.frame_number;
		wm_emit_input(&server, &release);
		assert(server.capture_client == NULL && server.state.cursor_shape == WM_CURSOR_ARROW);
		assert(server.state.frame_number == previous_frame + 1);
		assert_full_scene(&server);
	}
	wm_begin_capture(&server, target, WM_CAPTURE_MOVE);
	wm_present_input(&server, 0, 0);
	wm_emit_input(&server, &mouse_reset);
	assert(server.capture_client == NULL && server.state.cursor_shape == WM_CURSOR_ARROW);
	wm_begin_capture(&server, target, WM_CAPTURE_MOVE);
	wm_present_input(&server, 0, 0);
	wm_emit_input(&server, &key_reset);
	assert(server.capture_client == NULL && server.state.cursor_shape == WM_CURSOR_ARROW);
	server.state.cursor_x = 62;
	server.state.cursor_y = 140;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_RESIZE_HORIZONTAL);
	target->x = 128;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_ARROW);
	assert_full_scene(&server);
	target->x = 64;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	target->minimized = 1;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_ARROW);
	target->minimized = 0;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_RESIZE_HORIZONTAL);
	previous_frame = begin_drawing_measurement(&server);
	wm_begin_capture(&server, target, WM_CAPTURE_MOVE);
	server.state.mouse_buttons = 1;
	wm_present_input(&server, 0, 0);
	assert(server.state.frame_number == previous_frame + 1);
	assert(measured_work.written_bytes <= WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT * 4);
	measure_drawing = 0;
	assert_full_scene(&server);
	wm_emit_input(&server, &release);
	scanout_result = GUI_FLUSH_BUSY;
	wm_begin_capture(&server, target, WM_CAPTURE_MOVE);
	server.state.mouse_buttons = 1;
	wm_present_input(&server, 0, 0);
	assert(server.state.cursor_shape == WM_CURSOR_MOVE);
	assert(server.state.painted_cursor_shape == WM_CURSOR_RESIZE_HORIZONTAL);
	server.state.cursor_x = server.state.cursor_y = 12;
	wm_present_input(&server, 1, 0);
	scanout_result = GUI_FLUSH_COMPLETE;
	wm_emit_input(&server, &release);
	assert(server.state.cursor_shape == WM_CURSOR_ARROW);
	assert_full_scene(&server);
	for (size_t index = 0; index < sizeof(capture_cases) / sizeof(capture_cases[0]); index++) {
		wm_begin_capture(&server, target, capture_cases[index].operation);
		for (unsigned int corner = 0; corner < 4; corner++) {
			server.state.cursor_x = corner & 1 ? 319 : 0;
			server.state.cursor_y = corner & 2 ? 239 : 0;
			assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
			assert(server.state.cursor_shape == capture_cases[index].shape);
			assert(server.state.composite->pixels[(size_t)server.state.cursor_y *
				server.state.composite->stride + server.state.cursor_x] == WM_WINDOW_CONTENT_COLOR);
			assert_full_scene(&server);
		}
	}
	wm_cancel_capture(&server);
	server.fullscreen_window = target;
	target->fullscreen = 1;
	server.state.cursor_x = 62;
	server.state.cursor_y = 140;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_ARROW);
	server.fullscreen_window = NULL;
	target->fullscreen = 0;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_RESIZE_HORIZONTAL);
	client_destroy(target->client);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.cursor_shape == WM_CURSOR_ARROW);
	assert_full_scene(&server);
	cleanup_server(&server);
	puts("gui-wm-busy: cursor shapes track hover and capture with clipped incremental redraws");
}

static void test_drawing_work(void)
{
	static wm_server server;
	wm_window *target;
	wm_window *cover;
	gui_input_event pointer = { .type = GUI_INPUT_EVENT_MOUSE };
	uint64_t previous_frame;

	init_server(&server, 1024, 768);
	server.state.framebuffer = 123;
	server.state.scanout_stride = 1024 * 4;
	server.state.graphics = 1;
	server.state.composite = gui_surface_create(1024, 768);
	server.desktop_snapshot = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL && server.desktop_snapshot != NULL);
	server.desktop_snapshot_bytes = 1024 * 768 * 4;
	server.state.surface_bytes = server.desktop_snapshot_bytes;
	gui_fill_rect(server.desktop_snapshot, (gui_rect) { 0, 0, 1024, 768 }, 0x00112233);
	target = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	target->x = 144;
	target->y = 112;
	target->width = 320;
	target->height = 192;
	target->stack_order = 1;
	target->committed = gui_surface_create(320, 192);
	assert(target->committed != NULL);
	target->committed_bytes = 320 * 192 * 4;
	target->client->surface_bytes = target->committed_bytes;
	server.state.surface_bytes += target->committed_bytes;
	gui_fill_rect(target->committed, (gui_rect) { 0, 0, 320, 192 }, 0x00225688);
	scanout_result = GUI_FLUSH_COMPLETE;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	previous_frame = begin_drawing_measurement(&server);
	wm_emit_input(&server, &pointer);
	report_drawing_work("idle", &server, previous_frame, 0);
	previous_frame = begin_drawing_measurement(&server);
	pointer.mouse.dx = 32;
	wm_emit_input(&server, &pointer);
	assert(server.state.motion_pending);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	report_drawing_work("cursor", &server, previous_frame, 1);
	assert_full_scene(&server);
	previous_frame = begin_drawing_measurement(&server);
	assert(wm_move_window(&server, target, 152, 112) == 1);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	report_drawing_work("small-window-move", &server, previous_frame, 1);
	assert_full_scene(&server);
	stage(target, 1, 2);
	assert(target->staging != NULL);
	gui_fill_rect(target->staging, (gui_rect) { 0, 0, 320, 192 }, 0x00336699);
	previous_frame = begin_drawing_measurement(&server);
	commit_staged(target);
	report_drawing_work("visible-commit", &server, previous_frame, 1);
	assert_full_scene(&server);
	cover = init_client(&server, 1, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	cover->x = cover->y = 0;
	cover->width = 1024;
	cover->height = 768;
	cover->fullscreen = 1;
	cover->stack_order = 2;
	cover->committed = gui_surface_create(1024, 768);
	assert(cover->committed != NULL);
	cover->committed_bytes = 1024 * 768 * 4;
	cover->client->surface_bytes = cover->committed_bytes;
	server.state.surface_bytes += cover->committed_bytes;
	server.fullscreen_window = cover;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	stage(target, 1, 3);
	assert(target->staging != NULL);
	previous_frame = begin_drawing_measurement(&server);
	commit_staged(target);
	report_drawing_work("hidden-commit", &server, previous_frame, 0);
	stage(cover, 1, 2);
	assert(cover->staging != NULL);
	previous_frame = begin_drawing_measurement(&server);
	commit_staged(cover);
	report_drawing_work("fullscreen-commit", &server, previous_frame, 1);
	assert_full_scene(&server);
	assert(wm_load_cursor(&server.state) == 0);
	wm_damage_rect(&server.state, (gui_rect) { 0, 0, 64, 16 });
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	for (unsigned int index = 0; index < 20; index++) {
		server.state.cursor_x = (index * 173) % 1024;
		server.state.cursor_y = (index * 113) % 768;
		assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
		assert_full_scene(&server);
	}
	server.fullscreen_window = NULL;
	window_destroy(cover);
	for (unsigned int row = 0; row < target->height; row++)
		for (unsigned int column = 0; column < target->width; column++)
			target->committed->pixels[row * target->width + column] =
				(row * 397 + column * 173) & 0x00ffffff;
	wm_damage_rect(&server.state, wm_content_bounds(target));
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	cover = init_client(&server, 1, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	cover->x = 224;
	cover->y = 176;
	cover->stack_order = 3;
	cover->committed = gui_surface_create(cover->width, cover->height);
	assert(cover->committed != NULL);
	cover->committed_bytes = cover->width * cover->height * 4;
	cover->client->surface_bytes = cover->committed_bytes;
	server.state.surface_bytes += cover->committed_bytes;
	gui_fill_rect(cover->committed, (gui_rect) { 0, 0, cover->width, cover->height }, 0x00665544);
	for (unsigned int index = 0; index < 12; index++) {
		assert(wm_move_window(&server, target, (index * 137) % 1024 - 32,
			(index * 93) % 768 - 16) >= 0);
		assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
		assert_full_scene(&server);
	}
	window_destroy(cover);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	previous_frame = server.state.frame_number;
	scanout_result = GUI_FLUSH_BUSY;
	server.state.cursor_x = 17;
	server.state.cursor_y = 19;
	assert(wm_compose(&server) == GUI_FLUSH_BUSY);
	assert(server.state.frame_number == previous_frame && server.state.damage.count > 0);
	scanout_result = GUI_FLUSH_COMPLETE;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	previous_frame = server.state.frame_number;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE && server.state.frame_number == previous_frame);
	for (unsigned int index = 0; index <= GUI_REGION_MAX_RECTS; index++)
		wm_damage_rect(&server.state, (gui_rect) { index * 3, 0, 1, 1 });
	assert(server.state.damage.full);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	cleanup_server(&server);
	puts("gui-wm-busy: incremental damage matches full composition with bounded work");
	puts("gui-wm-busy: drawing work is counted independently of logical flushes");
}

static wm_window *drawing_window(wm_server *server, unsigned int index)
{
	wm_window *window = init_client(server, index, GUI_PROTOCOL_ENDPOINT_ORDINARY);

	window->x = 144;
	window->y = 112;
	window->width = 320;
	window->height = 192;
	window->stack_order = index + 1;
	window->committed = gui_surface_create(320, 192);
	assert(window->committed != NULL);
	window->committed_bytes = 320 * 192 * 4;
	window->client->surface_bytes = window->committed_bytes;
	server->state.surface_bytes += window->committed_bytes;
	gui_fill_rect(window->committed, (gui_rect) { 0, 0, 320, 192 }, 0x00200000 + index);
	return window;
}

static void test_sixteen_window_drawing(void)
{
	static wm_server server;
	wm_window *windows[16];
	gui_input_event motion = { .type = GUI_INPUT_EVENT_MOUSE, .mouse = { .dx = 32 } };
	uint64_t previous_frame;
	size_t surface_bytes_before;
	size_t record_bytes_before;
	uint64_t damage_pixels = (320 + 2 * WM_RESIZE_GRIP + 8) *
		(192 + WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP);

	init_server(&server, 1024, 768);
	server.state.graphics = 1;
	server.state.framebuffer = 123;
	server.state.scanout_stride = 4096;
	server.state.composite = gui_surface_create(1024, 768);
	server.desktop_snapshot = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL && server.desktop_snapshot != NULL);
	server.desktop_snapshot_bytes = server.state.frame_bytes;
	server.state.surface_bytes = server.desktop_snapshot_bytes;
	gui_fill_rect(server.desktop_snapshot, (gui_rect) { 0, 0, 1024, 768 }, 0x00112233);
	for (unsigned int index = 0; index < 16; index++)
		windows[index] = drawing_window(&server, index);
	assert(wm_load_cursor(&server.state) == 0);
	scanout_result = GUI_FLUSH_COMPLETE;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	previous_frame = begin_drawing_measurement(&server);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(server.state.frame_number == previous_frame && measured_work.written_bytes == 0);
	assert(measured_work.filled_pixels == 0 && measured_work.blitted_pixels == 0 && measured_work.writer_calls == 0);
	use_test_clock = 1;
	test_clock_ms = 1000;
	for (unsigned int index = 0; index < 16; index++)
		wm_emit_input(&server, &motion);
	assert(server.state.cursor_x == 512 && server.state.motion_pending);
	assert(server.state.frame_number == previous_frame && wm_motion_timeout(&server) == WM_MOTION_FRAME_MS);
	test_clock_ms += WM_MOTION_FRAME_MS;
	assert(wm_present_motion(&server) == 0);
	use_test_clock = 0;
	measure_drawing = 0;
	assert(server.state.frame_number == previous_frame + 1 && !server.state.motion_pending);
	assert(measured_work.written_bytes == 2 * WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT * 4);
	assert(measured_work.flush_requests == 2 && measured_work.writer_calls == 2 * WM_CURSOR_HEIGHT);
	assert(measured_work.filled_pixels <= 2 * WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT);
	assert(measured_work.blitted_pixels <= 2 * WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT);
	assert_full_scene(&server);
	stage(windows[0], 1, 2);
	gui_fill_rect(windows[0]->staging, (gui_rect) { 0, 0, 320, 192 }, 0x00335566);
	previous_frame = begin_drawing_measurement(&server);
	commit_staged(windows[0]);
	measure_drawing = 0;
	assert(windows[0]->committed->pixels[0] == 0x00335566);
	assert(server.state.frame_number == previous_frame && measured_work.written_bytes == 0);
	assert(measured_work.filled_pixels == 0 && measured_work.blitted_pixels == 0 && measured_work.writer_calls == 0);
	assert_full_scene(&server);
	previous_frame = begin_drawing_measurement(&server);
	assert(wm_move_window(&server, windows[15], 152, 112) == 1);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	measure_drawing = 0;
	assert(server.state.frame_number == previous_frame + 1 && measured_work.written_bytes <= damage_pixels * 4);
	assert(measured_work.filled_pixels <= damage_pixels * 9 && measured_work.blitted_pixels <= damage_pixels * 3);
	assert(measured_work.flush_requests == 1 && measured_work.writer_calls <= 192 + WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP);
	assert_full_scene(&server);
	surface_bytes_before = server.state.surface_bytes;
	record_bytes_before = server.record_bytes;
	for (unsigned int index = 0; index < 16; index++) {
		int descriptor = windows[index]->client->fd;

		wm_disconnect_client(&server, windows[index]->client);
		assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
		windows[index] = drawing_window(&server, index + 16);
		assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
		assert(server.client_count == 16 && server.record_bytes == record_bytes_before);
		assert(server.state.surface_bytes == surface_bytes_before);
		for (unsigned int peer = 0; peer < 16; peer++) {
			gui_color expected = 0x00200000 + peer + (peer <= index ? 16 : 0);

			for (unsigned int pixel = 0; pixel < 320 * 192; pixel++)
				assert(windows[peer]->committed->pixels[pixel] == expected);
		}
		assert_full_scene(&server);
	}
	cleanup_server(&server);
	puts("gui-wm-busy: sixteen windows preserve bounded drawing, motion and reclaimed capacity");
}

static void test_multiwindow_capacity_cycles(void)
{
	static wm_server server;
	wm_window *healthy;
	size_t base_surfaces;
	size_t base_records;
	uint32_t previous_identity = 0;

	init_server(&server, 1024, 768);
	server.state.graphics = 1;
	server.state.framebuffer = 123;
	server.state.scanout_stride = 4096;
	server.state.composite = gui_surface_create(1024, 768);
	server.desktop_snapshot = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL && server.desktop_snapshot != NULL);
	server.desktop_snapshot_bytes = server.state.frame_bytes;
	server.state.surface_bytes = server.desktop_snapshot_bytes;
	gui_fill_rect(server.desktop_snapshot, (gui_rect) { 0, 0, 1024, 768 }, 0x00112233);
	healthy = drawing_window(&server, server.next_window_id);
	healthy->x = 680;
	healthy->y = 80;
	assert(wm_poll_resize(&server, 2) == 0);
	assert(wm_load_cursor(&server.state) == 0);
	scanout_result = GUI_FLUSH_COMPLETE;
	base_surfaces = server.state.surface_bytes;
	base_records = server.record_bytes;
	for (unsigned int cycle = 0; cycle < 16; cycle++) {
		wm_client *peer = wm_add_client(&server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
		wm_window *windows[14];
		int descriptor;

		assert(peer != NULL);
		peer->hello = 1;
		for (unsigned int index = 0; index < 14; index++) {
			create_record(peer, 1000 + cycle * 14 + index, 96, 64);
			windows[index] = peer->windows;
			assert(windows[index] != NULL && windows[index]->window_id > previous_identity);
			previous_identity = windows[index]->window_id;
			windows[index]->x = 128;
			windows[index]->y = 96;
			fullscreen_stage(windows[index], 0x00200000 + index);
			commit_staged(windows[index]);
			drain_peer(peer);
		}
		assert(server.client_count == 2 && peer->surface_bytes == 14 * 96 * 64 * 4);
		assert(server.state.surface_bytes == base_surfaces + peer->surface_bytes);
		assert(peer->record_bytes == sizeof(wm_client) + sizeof(struct pollfd) +
			sizeof(wm_client *) + 14 * sizeof(wm_window));
		assert(server.record_bytes == base_records + sizeof(wm_client) + 14 * sizeof(wm_window));
		wm_set_input_owner(&server, windows[13]);
		assert(wm_toggle_fullscreen(&server, windows[13]) == 1);
		fullscreen_stage(windows[13], 0x00448866);
		commit_staged(windows[13]);
		assert(server.fullscreen_window == windows[13] && !windows[13]->mode_pending);
		assert(server.state.surface_bytes == base_surfaces + 13 * 96 * 64 * 4 + 1024 * 768 * 4);
		assert_full_scene(&server);
		drain_peer(peer);
		assert(wm_toggle_fullscreen(&server, windows[13]) == 1);
		fullscreen_stage(windows[13], 0x0020000d);
		commit_staged(windows[13]);
		assert(server.fullscreen_window == NULL && !windows[13]->mode_pending);
		assert(windows[13]->x == 128 && windows[13]->y == 96);
		assert(windows[13]->width == 96 && windows[13]->height == 64);
		assert(server.state.surface_bytes == base_surfaces + 14 * 96 * 64 * 4);
		for (unsigned int index = 0; index < 14; index++) {
			for (unsigned int pixel = 0; pixel < 96 * 64; pixel++)
				assert(windows[index]->committed->pixels[pixel] == 0x00200000 + index);
		}
		for (unsigned int pixel = 0; pixel < 320 * 192; pixel++)
			assert(healthy->committed->pixels[pixel] == 0x00200000);
		assert_full_scene(&server);
		descriptor = peer->fd;
		wm_disconnect_client(&server, peer);
		assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
		assert(server.client_count == 1 && server.record_bytes == base_records);
		assert(server.state.surface_bytes == base_surfaces);
		assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
		assert_full_scene(&server);
	}
	cleanup_server(&server);
	puts("gui-wm-busy: sixteen multiwindow fullscreen cycles reclaim exact metadata and surface charges");
}

static void test_motion_batch(void)
{
	static wm_server server;
	wm_window *target;
	gui_input_event pointer = { .type = GUI_INPUT_EVENT_MOUSE, .mouse = { .dx = 2 } };
	gui_input_event key = { .type = GUI_INPUT_EVENT_KEY,
		.key = { .scancode = 0x1e, .pressed = 1 } };
	uint64_t previous_frame;
	uint64_t deadline;
	unsigned int previous_calls;
	int old_x;

	init_server(&server, 1024, 768);
	server.state.graphics = 1;
	server.state.framebuffer = 123;
	server.state.scanout_stride = 4096;
	server.state.composite = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL && wm_load_cursor(&server.state) == 0);
	server.state.cursor_x = server.state.cursor_y = 16;
	target = init_client(&server, 0, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	target->width = 320;
	target->height = 192;
	target->stack_order = 1;
	fullscreen_stage(target, 0x00225688);
	scanout_result = GUI_FLUSH_COMPLETE;
	commit_staged(target);
	use_test_clock = 1;
	test_clock_ms = 1000;
	previous_frame = begin_drawing_measurement(&server);
	for (unsigned int index = 0; index < 16; index++)
		wm_emit_input(&server, &pointer);
	assert(server.state.cursor_x == 48 && server.state.frame_number == previous_frame);
	assert(server.state.damage.count == 0 && wm_motion_timeout(&server) == 16);
	test_clock_ms += 15;
	assert(wm_present_motion(&server) == 0 && wm_motion_timeout(&server) == 1);
	assert(server.state.frame_number == previous_frame && measured_work.writer_calls == 0);
	test_clock_ms++;
	assert(wm_present_motion(&server) == 0 && server.state.frame_number == previous_frame + 1);
	assert(measured_work.filled_pixels == 512 && measured_work.written_bytes == 2048);
	assert(measured_work.flush_requests == 2 && !server.state.motion_pending);
	measure_drawing = 0;
	assert_full_scene(&server);
	pointer.mouse.dx = 0;
	previous_frame = server.state.frame_number;
	for (unsigned int index = 0; index < 16; index++)
		wm_emit_input(&server, &pointer);
	assert(!server.state.motion_pending && wm_motion_timeout(&server) == -1);
	assert(server.state.frame_number == previous_frame);
	pointer.mouse.dx = -2000;
	wm_emit_input(&server, &pointer);
	pointer.mouse.dx = 10;
	wm_emit_input(&server, &pointer);
	assert(server.state.cursor_x == 10);
	test_clock_ms += 16;
	assert(wm_present_motion(&server) == 0);
	assert_full_scene(&server);
	server.state.cursor_x = target->x + 8;
	server.state.cursor_y = target->y - 12;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	pointer.mouse.dx = 0;
	pointer.mouse.buttons = 1;
	wm_emit_input(&server, &pointer);
	assert(server.capture_client == target && server.input_owner == target);
	drain_output(target);
	previous_frame = server.state.frame_number;
	old_x = target->x;
	pointer.mouse.dx = 2;
	for (unsigned int index = 0; index < 16; index++)
		wm_emit_input(&server, &pointer);
	assert(target->x == old_x + 32 && server.state.frame_number == previous_frame);
	wm_emit_input(&server, &key);
	drain_output(target);
	assert(captured_bytes == GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_input_event));
	assert(get_u32(captured_output + GUI_PROTOCOL_HEADER_SIZE + 8) == 'a');
	assert(server.state.frame_number == previous_frame);
	pointer.mouse.dx = 0;
	pointer.mouse.buttons = 0;
	wm_emit_input(&server, &pointer);
	assert(server.capture_client == NULL && target->x == old_x + 32);
	assert(server.state.frame_number == previous_frame + 1 && !server.state.motion_pending);
	assert_full_scene(&server);
	drain_output(target);
	previous_frame = server.state.frame_number;
	busy_input = 1;
	input_reads = 0;
	for (unsigned int iteration = 0; iteration < 5; iteration++) {
		unsigned int previous_reads = input_reads;

		assert(wm_read_input(&server, 900, 1) == 0);
		assert(input_reads - previous_reads == WM_INPUT_READ_QUANTUM);
		wm_emit_input(&server, &key);
		drain_output(target);
		assert(captured_bytes > 0 && target->client->hello == 1);
	}
	assert(input_reads == 20 && server.state.frame_number == previous_frame + 1);
	assert(wm_read_input(&server, 901, 0) == 0 && input_reads == 24);
	drain_output(target);
	assert(captured_bytes == 64 * (GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_input_event)));
	busy_input = 0;
	test_clock_ms += 16;
	assert(wm_present_motion(&server) == 0);
	assert_full_scene(&server);
	pointer.mouse.dx = 4;
	wm_emit_input(&server, &pointer);
	deadline = server.state.motion_deadline_ms;
	previous_frame = server.state.frame_number;
	previous_calls = scanout_calls;
	scanout_result = GUI_FLUSH_BUSY;
	test_clock_ms = deadline;
	assert(wm_present_motion(&server) == 0 && server.state.scanout_busy);
	assert(server.state.damage.count > 0 && server.state.frame_number == previous_frame);
	for (unsigned int index = 0; index < 16; index++) {
		test_clock_ms += 16;
		wm_emit_input(&server, &pointer);
		assert(wm_motion_timeout(&server) == -1 && wm_present_motion(&server) == 0);
	}
	assert(scanout_calls == previous_calls + 1);
	scanout_result = GUI_FLUSH_COMPLETE;
	server.state.scanout_busy = 0;
	gui_region_mark_full(&server.state.damage);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	wm_emit_input(&server, &pointer);
	server.state.graphics = 0;
	previous_frame = server.state.frame_number;
	old_x = server.state.cursor_x;
	test_clock_ms += 16;
	assert(wm_motion_timeout(&server) == -1 && wm_present_motion(&server) == 0);
	wm_emit_input(&server, &pointer);
	assert(server.state.frame_number == previous_frame && server.state.cursor_x == (uint32_t)old_x);
	server.state.graphics = 1;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	pointer.type = GUI_INPUT_EVENT_MOUSE_RESET;
	wm_emit_input(&server, &pointer);
	assert(!server.state.mouse_buttons && !server.capture_client);
	use_test_clock = 0;
	cleanup_server(&server);
	puts("gui-wm-busy: motion deadlines preserve ordered input and bounded dispatch");
}

static void test_patch_begin(wm_window *window, uint64_t base, uint64_t frame, gui_rect rectangle)
{
	uint8_t bytes[sizeof(gui_protocol_wire_surface_patch_begin)] = { 0 };

	put_u32(bytes, window->window_id);
	put_u64(bytes + 4, window->configure_serial);
	put_u64(bytes + 12, base);
	put_u64(bytes + 20, frame);
	put_u32(bytes + 28, rectangle.x);
	put_u32(bytes + 32, rectangle.y);
	put_u32(bytes + 36, rectangle.w);
	put_u32(bytes + 40, rectangle.h);
	assert(client_stage_patch(window, bytes) == 0);
}

static void test_patch_data(wm_window *window, uint64_t frame, uint32_t offset, uint32_t count)
{
	uint8_t bytes[sizeof(gui_protocol_wire_surface_data) + 64 * sizeof(gui_color)] = { 0 };

	assert(count <= 64);
	put_u32(bytes, window->window_id);
	put_u64(bytes + 4, window->configure_serial);
	put_u64(bytes + 12, frame);
	put_u32(bytes + 20, offset);
	put_u32(bytes + 24, count);
	for (unsigned int index = 0; index < count; index++)
		put_u32(bytes + sizeof(gui_protocol_wire_surface_data) + index * sizeof(gui_color), 0x00cc8844);
	assert(client_surface_data(window, bytes, sizeof(gui_protocol_wire_surface_data) + count * sizeof(gui_color)) == 0);
}

static void test_patch_finish(wm_window *window, uint64_t frame, int cancel)
{
	uint8_t bytes[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	put_u32(bytes, window->window_id);
	put_u64(bytes + 4, window->configure_serial);
	put_u64(bytes + 12, frame);
	if (cancel)
		client_cancel_surface(window, bytes);
	else
		assert(client_commit_surface(window, bytes) == 0);
}

static void assert_patch_error(wm_window *window, int status)
{
	gui_protocol_header header;

	drain_output(window);
	assert(gui_protocol_decode_header(captured_output, captured_bytes, &header) == 0);
	assert(header.type == GUI_PROTOCOL_ERROR);
	assert((int32_t)get_u32(captured_output + GUI_PROTOCOL_HEADER_SIZE) == status);
}

static wm_window *test_patch_window(wm_server *server, uint16_t minor)
{
	wm_client *peer = wm_add_client(server, dup(STDOUT_FILENO), GUI_PROTOCOL_ENDPOINT_ORDINARY);
	gui_protocol_header hello = { .type = GUI_PROTOCOL_HELLO };
	gui_protocol_header capabilities = { .type = GUI_PROTOCOL_CAPABILITIES };
	uint8_t payload[sizeof(gui_protocol_wire_window_create)] = { 0 };
	wm_window *window;

	assert(peer != NULL);
	put_u16(payload, 1);
	put_u16(payload + 2, minor);
	client_message(peer, &hello, payload, 4);
	assert(peer->hello == 1 && peer->protocol_minor == minor);
	captured_bytes = 0;
	assert(gui_protocol_output_flush(peer->output, capture_output, NULL) == GUI_PROTOCOL_OUTPUT_EMPTY);
	assert(get_u16(captured_output + GUI_PROTOCOL_HEADER_SIZE + 2) == minor);
	put_u32(payload, GUI_PROTOCOL_CAP_SURFACE_PATCH | 2);
	client_message(peer, &capabilities, payload, 4);
	captured_bytes = 0;
	assert(gui_protocol_output_flush(peer->output, capture_output, NULL) == GUI_PROTOCOL_OUTPUT_EMPTY);
	assert(peer->capabilities == (minor ? GUI_PROTOCOL_CAP_SURFACE_PATCH : 0));
	assert(get_u32(captured_output + GUI_PROTOCOL_HEADER_SIZE) == (minor ? 1U : (uint32_t)-EPROTONOSUPPORT));
	memset(payload, 0, sizeof(payload));
	put_u32(payload, 1);
	put_u32(payload + 4, 64);
	put_u32(payload + 8, 48);
	client_create_window(peer, payload, sizeof(payload), 0);
	window = peer->windows;
	assert(window != NULL);
	drain_output(window);
	stage(window, window->configure_serial, 2);
	assert(window->staging != NULL);
	gui_fill_rect(window->staging, (gui_rect) { 0, 0, 64, 48 }, 0x00225688);
	commit_staged(window);
	drain_output(window);
	return window;
}

static void test_atomic_patches(void)
{
	static wm_server server;
	wm_window *legacy;
	wm_window *window;
	wm_window *cover;
	gui_rect rectangle = { 4, 3, 8, 5 };
	gui_surface *base;
	uint64_t previous_frame;
	size_t previous_bytes;
	size_t limit;

	init_server(&server, 1024, 768);
	server.state.graphics = 1;
	server.state.framebuffer = 123;
	server.state.scanout_stride = 4096;
	server.state.composite = gui_surface_create(1024, 768);
	assert(server.state.composite != NULL);
	scanout_result = GUI_FLUSH_COMPLETE;
	legacy = test_patch_window(&server, 0);
	window = test_patch_window(&server, 1);
	drain_output(legacy);
	base = window->committed;
	previous_bytes = server.state.surface_bytes;
	test_patch_begin(legacy, 2, 3, rectangle);
	assert_patch_error(legacy, -EPROTONOSUPPORT);
	test_patch_begin(window, 99, 3, rectangle);
	assert_patch_error(window, -ESTALE);
	test_patch_begin(window, 2, 3, (gui_rect) { -1, 0, 8, 5 });
	assert_patch_error(window, -EINVAL);
	test_patch_begin(window, 2, 3, (gui_rect) { 60, 0, 8, 5 });
	assert_patch_error(window, -EINVAL);
	test_patch_begin(window, 2, 3, rectangle);
	assert(window->staging != NULL && window->committed == base);
	assert(server.state.surface_bytes == previous_bytes + 64 * 48 * 4);
	assert(!memcmp(base->pixels, window->staging->pixels, 64 * 48 * 4));
	test_patch_begin(window, 2, 4, rectangle);
	assert_patch_error(window, -EBUSY);
	test_patch_data(window, 3, 0, 17);
	drain_output(window);
	test_patch_finish(window, 3, 0);
	assert_patch_error(window, -EINVAL);
	assert(window->committed == base && window->staging_pixels == 17);
	test_patch_data(window, 3, 0, 17);
	assert_patch_error(window, -EINVAL);
	test_patch_data(window, 3, 17, 23);
	drain_output(window);
	previous_frame = begin_drawing_measurement(&server);
	test_patch_finish(window, 3, 0);
	measure_drawing = 0;
	assert(server.state.frame_number == previous_frame + 1 && measured_work.written_bytes == 160);
	assert(measured_work.flush_requests == 1 && measured_work.writer_calls == 5);
	assert(window->committed_frame_serial == 3 && window->staging == NULL);
	for (unsigned int row = 0; row < 48; row++)
		for (unsigned int column = 0; column < 64; column++)
			assert(window->committed->pixels[row * 64 + column] ==
				(row >= 3 && row < 8 && column >= 4 && column < 12 ? 0x00cc8844 : 0x00225688));
	assert_full_scene(&server);
	cover = init_client(&server, 2, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	cover->x = window->x;
	cover->y = window->y;
	cover->stack_order = ++server.next_stack_order;
	stage(cover, 1, 2);
	assert(cover->staging != NULL);
	gui_fill_rect(cover->staging, (gui_rect) { 0, 0, 64, 48 }, 0x00665544);
	commit_staged(cover);
	assert(wm_window_obscured(&server, window));
	test_patch_begin(window, 3, 4, (gui_rect) { 20, 10, 8, 5 });
	test_patch_data(window, 4, 0, 40);
	drain_output(window);
	previous_frame = begin_drawing_measurement(&server);
	test_patch_finish(window, 4, 0);
	measure_drawing = 0;
	assert(server.state.frame_number == previous_frame && measured_work.writer_calls == 0);
	window_destroy(cover);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert(window->committed->pixels[10 * 64 + 20] == 0x00cc8844);
	assert_full_scene(&server);
	limit = server.state.surface_limit;
	server.state.surface_limit = server.state.surface_bytes + window->committed_bytes;
	test_patch_begin(window, 4, 5, rectangle);
	assert_patch_error(window, -ENOMEM);
	assert(window->staging == NULL && window->committed_frame_serial == 4);
	server.state.surface_limit = limit;
	test_patch_begin(window, 4, 5, rectangle);
	test_patch_finish(window, 5, 1);
	assert(window->staging == NULL && window->committed_frame_serial == 4);
	test_patch_begin(window, 4, 5, rectangle);
	assert_patch_error(window, -ESTALE);
	fail_surface_allocation = 1;
	test_patch_begin(window, 4, 6, rectangle);
	assert_patch_error(window, -ENOMEM);
	assert(window->staging == NULL && window->configure_serial == 2 && window->committed_frame_serial == 4);
	test_patch_begin(window, 4, 7, rectangle);
	assert_patch_error(window, -ESTALE);
	stage(window, 2, 8);
	assert(window->staging != NULL);
	gui_fill_rect(window->staging, (gui_rect) { 0, 0, 64, 48 }, 0x00225688);
	commit_staged(window);
	test_patch_begin(window, 8, 9, rectangle);
	test_patch_data(window, 9, 0, 40);
	drain_output(window);
	base = window->committed;
	scanout_result = GUI_FLUSH_ERROR;
	test_patch_finish(window, 9, 0);
	assert_patch_error(window, -EIO);
	assert(window->committed == base && window->committed_frame_serial == 8 && window->configure_serial == 3);
	scanout_result = GUI_FLUSH_COMPLETE;
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	assert(legacy->client->hello == 1 && legacy->committed != NULL);
	stage(window, 3, 10);
	assert(window->staging != NULL);
	gui_fill_rect(window->staging, (gui_rect) { 0, 0, 64, 48 }, 0x00225688);
	commit_staged(window);
	test_patch_begin(window, 10, 11, rectangle);
	test_patch_data(window, 11, 0, 7);
	drain_output(window);
	assert(wm_resize_window(&server, window, window->x, window->y, 80, 48) == 1);
	assert(window->staging == NULL && window->reserved_bytes == 80 * 48 * 4);
	drain_output(window);
	test_patch_begin(window, 10, 12, rectangle);
	assert_patch_error(window, -ESTALE);
	{
		int descriptor = window->client->fd;

		wm_disconnect_client(&server, window->client);
		assert(fcntl(descriptor, F_GETFD) < 0 && errno == EBADF);
	}
	assert(server.state.surface_bytes == legacy->committed_bytes);
	assert(legacy->client->hello == 1 && legacy->committed->pixels[0] == 0x00225688);
	assert(wm_compose(&server) == GUI_FLUSH_COMPLETE);
	assert_full_scene(&server);
	cleanup_server(&server);
	puts("gui-wm-busy: negotiated patches preserve atomic pixels, identities and charges");
}

typedef struct {
	uint8_t bytes[32768];
	size_t count;
	size_t stop_at;
	size_t short_write;
	unsigned int calls;
	int interrupt;
} upload_sink;

static ssize_t upload_test_write(void *context, const void *bytes, size_t count)
{
	upload_sink *sink = context;

	sink->calls++;
	if (sink->interrupt) {
		sink->interrupt = 0;
		errno = EINTR;
		return -1;
	}
	if (sink->stop_at && sink->count >= sink->stop_at) {
		errno = EAGAIN;
		return -1;
	}
	if (sink->stop_at && count > sink->stop_at - sink->count)
		count = sink->stop_at - sink->count;
	if (sink->short_write && count > sink->short_write)
		count = sink->short_write;
	assert(count <= sizeof(sink->bytes) - sink->count);
	memcpy(sink->bytes + sink->count, bytes, count);
	sink->count += count;
	return count;
}

static int upload_test_ack(terminal_upload *upload, uint64_t configure, uint64_t serial,
			    uint32_t offset, uint32_t count)
{
	uint8_t payload[sizeof(gui_protocol_wire_surface_data_reply)];

	put_u32(payload, upload->window);
	put_u64(payload + 4, configure);
	put_u64(payload + 12, serial);
	put_u32(payload + 20, offset);
	put_u32(payload + 24, count);
	return terminal_upload_reply(upload, payload, sizeof(payload));
}

static void upload_test_pump(terminal_upload *upload, upload_sink *sink)
{
	unsigned int previous_calls = sink->calls;

	assert(terminal_upload_write(upload, upload_test_write, sink) == 0);
	assert(sink->calls - previous_calls <= 4);
	assert(upload->outstanding_bytes <= TERMINAL_UPLOAD_WINDOW_BYTES);
	assert(upload->chunk_count <= TERMINAL_UPLOAD_CHUNKS);
}

static void assert_upload_pixels(const upload_sink *sink, const gui_surface *source, gui_rect rectangle,
				 uint64_t serial, unsigned int expected_chunks)
{
	unsigned int chunks = 0;
	uint32_t pixels = 0;

	for (size_t offset = 0; offset < sink->count;) {
		gui_protocol_header header;
		const uint8_t *payload = sink->bytes + offset + GUI_PROTOCOL_HEADER_SIZE;

		assert(gui_protocol_decode_header(sink->bytes + offset, sink->count - offset, &header) == 0);
		assert(header.length <= sink->count - offset);
		if (header.type == GUI_PROTOCOL_SURFACE_DATA) {
			uint32_t count = get_u32(payload + 24);

			assert(get_u64(payload + 12) == serial && get_u32(payload + 20) == pixels);
			assert(header.length == GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_data) + count * 4);
			for (uint32_t index = 0; index < count; index++) {
				size_t row = rectangle.y + (pixels + index) / rectangle.w;
				size_t column = rectangle.x + (pixels + index) % rectangle.w;

				assert(get_u32(payload + sizeof(gui_protocol_wire_surface_data) + index * 4) ==
					source->pixels[row * source->stride + column]);
			}
			pixels += count;
			chunks++;
		}
		offset += header.length;
	}
	assert(chunks == expected_chunks && pixels == (uint32_t)rectangle.w * rectangle.h);
}

static void test_upload_window(void)
{
	static terminal_upload upload;
	static upload_sink sink;
	gui_surface *source = gui_surface_create(64, 64);
	gui_rect full = { 0, 0, 64, 64 };
	gui_rect cell = { 16, 16, 16, 16 };
	size_t previous_bytes;

	assert(source != NULL);
	for (unsigned int index = 0; index < 4096; index++)
		source->pixels[index] = index ^ 0x00556677;
	assert(terminal_upload_start(&upload, 1, 1, 0, 1, source, full, 0) == 0);
	assert(upload_test_ack(&upload, 1, 1, 0, 1000) < 0);
	while (upload.chunk_count < 4)
		upload_test_pump(&upload, &sink);
	assert(upload.next_pixel == 4000 && upload.outstanding_bytes == 16144);
	assert(upload.peak_outstanding_bytes == 16144 && !terminal_upload_writable(&upload));
	previous_bytes = upload.outstanding_bytes;
	assert(upload_test_ack(&upload, 2, 1, 0, 1000) < 0 && upload.outstanding_bytes == previous_bytes);
	assert(upload_test_ack(&upload, 1, 1, 0, 999) < 0 && upload.outstanding_bytes == previous_bytes);
	assert(upload_test_ack(&upload, 1, 1, 0, 1000) == 0);
	previous_bytes = upload.outstanding_bytes;
	assert(upload_test_ack(&upload, 1, 1, 0, 1000) == 0 && upload.outstanding_bytes == previous_bytes);
	upload_test_pump(&upload, &sink);
	assert(upload.next_pixel == 4096 && upload.chunk_count == 4);
	for (uint32_t offset = 1000; offset < 4000; offset += 1000)
		assert(upload_test_ack(&upload, 1, 1, offset, 1000) == 0);
	assert(upload_test_ack(&upload, 1, 1, 4000, 96) == 0);
	upload_test_pump(&upload, &sink);
	assert(terminal_upload_ready(&upload) && upload.completed && upload.completed_serial == 1);
	assert(sink.count == 16636);
	assert_upload_pixels(&sink, source, full, 1, 5);
	memset(&sink, 0, sizeof(sink));
	sink.short_write = 3;
	sink.interrupt = 1;
	assert(terminal_upload_start(&upload, 1, 1, 1, 2, source, cell, 1) == 0);
	upload_test_pump(&upload, &sink);
	assert(sink.count == 0 && upload.frame_written == 0);
	upload_test_pump(&upload, &sink);
	assert(upload.frame_written == 12);
	terminal_upload_cancel(&upload, 1);
	while (terminal_upload_writable(&upload))
		upload_test_pump(&upload, &sink);
	assert(terminal_upload_ready(&upload) && !upload.completed && sink.count == 80);
	memset(&sink, 0, sizeof(sink));
	sink.stop_at = 54;
	assert(terminal_upload_start(&upload, 1, 1, 1, 3, source, cell, 1) == 0);
	upload_test_pump(&upload, &sink);
	assert(upload.frame_type == GUI_PROTOCOL_SURFACE_DATA && upload.frame_written == 2);
	assert(upload_test_ack(&upload, 1, 3, 0, 256) < 0);
	terminal_upload_cancel(&upload, 1);
	sink.stop_at = 0;
	while (terminal_upload_writable(&upload))
		upload_test_pump(&upload, &sink);
	assert(!terminal_upload_ready(&upload) && upload.chunk_count == 1);
	assert(terminal_upload_start(&upload, 1, 1, 1, 4, source, cell, 1) < 0);
	assert(terminal_upload_data_error(&upload) == 0 && terminal_upload_ready(&upload));
	assert_upload_pixels(&sink, source, cell, 3, 1);
	memset(&sink, 0, sizeof(sink));
	assert(terminal_upload_start(&upload, 1, 1, 1, 4, source, cell, 1) == 0);
	assert(upload_test_ack(&upload, 1, 3, 0, 256) == 0 && upload.chunk_count == 0);
	upload_test_pump(&upload, &sink);
	assert(upload.chunk_count == 1 && upload.outstanding_bytes == 1060);
	assert(upload_test_ack(&upload, 1, 4, 0, 256) == 0);
	upload_test_pump(&upload, &sink);
	assert(sink.count == 1140 && terminal_upload_ready(&upload));
	assert_upload_pixels(&sink, source, cell, 4, 1);
	gui_surface_destroy(source);
	puts("gui-wm-busy: upload window caps encoded bytes and survives short, delayed and stale replies");
}

static uint8_t held_upload_replies[TERMINAL_UPLOAD_CHUNKS *
	(GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_data_reply))];
static size_t held_upload_bytes;
static int hold_upload_replies;
static unsigned int upload_commits;
static unsigned int upload_full_begins;
static unsigned int upload_patch_begins;
static size_t upload_wire_bytes;
static int upload_legacy;
static unsigned int upload_legacy_rejections;

static ssize_t upload_peer_write(void *context, const void *bytes, size_t count)
{
	wm_client *peer = context;
	gui_protocol_header header;

	if (hold_upload_replies && gui_protocol_decode_header(bytes, count, &header) == 0 &&
	    header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY) {
		assert(header.length == count);
		assert(count <= sizeof(held_upload_replies) - held_upload_bytes);
		memcpy(held_upload_replies + held_upload_bytes, bytes, count);
		held_upload_bytes += count;
		return count;
	}
	return write(peer->fd, bytes, count);
}

static void upload_peer_message(void *context, const gui_protocol_header *header,
				const void *payload, size_t bytes)
{
	wm_client *peer = context;

	if (upload_legacy) {
		assert(header->type != GUI_PROTOCOL_CAPABILITIES && header->type != GUI_PROTOCOL_SURFACE_PATCH_BEGIN);
		if (header->type == GUI_PROTOCOL_HELLO && get_u32(payload) >> 16 != 0) {
			upload_legacy_rejections++;
			client_reject(peer, GUI_PROTOCOL_HELLO, -EPROTONOSUPPORT);
			return;
		}
	}
	if (header->type == GUI_PROTOCOL_SURFACE_BEGIN)
		upload_full_begins++;
	if (header->type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN)
		upload_patch_begins++;
	if (header->type == GUI_PROTOCOL_SURFACE_BEGIN || header->type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN ||
	    header->type == GUI_PROTOCOL_SURFACE_DATA || header->type == GUI_PROTOCOL_SURFACE_COMMIT)
		upload_wire_bytes += GUI_PROTOCOL_HEADER_SIZE + bytes;
	client_message(context, header, payload, bytes);
	if (header->type == GUI_PROTOCOL_SURFACE_COMMIT && peer->windows != NULL &&
	    peer->windows->committed_frame_serial == get_u64((const uint8_t *)payload + 12))
		upload_commits++;
}

static int upload_peer_pump(wm_client *peer)
{
	uint8_t bytes[4096];
	ssize_t count = recv(peer->fd, bytes, sizeof(bytes), 0);
	int messages;

	assert(count >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
	if (count > 0)
		assert(gui_protocol_input_append(peer->input, bytes, count) == 0);
	messages = gui_protocol_input_drain(peer->input, upload_peer_message, peer);
	assert(messages >= 0 && peer->hello >= 0);
	if (count == 0 && (messages < 8 || !gui_protocol_input_pending(peer->input))) {
		wm_disconnect_client(peer->server, peer);
		return 1;
	}
	if (gui_protocol_output_flush(peer->output, upload_peer_write, peer) == GUI_PROTOCOL_OUTPUT_ERROR) {
		wm_disconnect_client(peer->server, peer);
		return 1;
	}
	return 0;
}

static int upload_log_contains(const char *path, const char *marker)
{
	static char bytes[32769];
	FILE *file = fopen(path, "r");
	size_t count;

	if (file == NULL)
		return 0;
	count = fread(bytes, 1, sizeof(bytes) - 1, file);
	bytes[count] = 0;
	assert(fclose(file) == 0);
	return strstr(bytes, marker) != NULL;
}

static wm_client *start_upload_terminal(wm_server *server, const char *command, const char *log,
				      int *listener, pid_t *child)
{
	struct pollfd ready;
	int descriptor;

	init_server(server, 1024, 768);
	server->state.graphics = 1;
	server->state.framebuffer = 123;
	server->state.scanout_stride = 4096;
	server->state.composite = gui_surface_create(1024, 768);
	assert(server->state.composite != NULL);
	scanout_result = GUI_FLUSH_COMPLETE;
	measure_drawing = 0;
	unlink(WM_SOCKET_PATH);
	*listener = bind_listener();
	assert(*listener >= 0);
	held_upload_bytes = upload_commits = upload_full_begins = upload_patch_begins = upload_wire_bytes = 0;
	*child = fork();
	assert(*child >= 0);
	if (*child == 0) {
		int output = open(log, O_CREAT | O_TRUNC | O_WRONLY, 0600);

		assert(output >= 0 && dup2(output, STDOUT_FILENO) == STDOUT_FILENO && dup2(output, STDERR_FILENO) == STDERR_FILENO);
		close(output);
		execl(WM_TERMINAL_PATH, WM_TERMINAL_PATH, "--command", command, NULL);
		_exit(127);
	}
	ready = (struct pollfd) { .fd = *listener, .events = POLLIN };
	assert(poll(&ready, 1, 10000) == 1);
	descriptor = accept(*listener, NULL, NULL);
	assert(descriptor >= 0 && fcntl(descriptor, F_SETFL, O_NONBLOCK) == 0);
	return wm_add_client(server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY);
}

static void finish_upload_terminal(wm_server *server, wm_client *peer, int listener, pid_t child,
				   const char *finish)
{
	int exited = 0;
	int disconnected = 0;
	int status = 0;

	touch_marker(finish);
	for (unsigned int attempt = 0; !exited || !disconnected; attempt++) {
		assert(attempt < 20000);
		if (!disconnected)
			disconnected = upload_peer_pump(peer);
		if (!exited) {
			pid_t result = waitpid(child, &status, WNOHANG);

			assert(result >= 0);
			exited = result == child;
		}
		assert(usleep(1000) == 0);
	}
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && server->state.surface_bytes == 0);
	close(listener);
	unlink(WM_SOCKET_PATH);
	cleanup_server(server);
}

static void test_live_upload_progress(void)
{
	static wm_server server;
	wm_client *peer;
	wm_window *window;
	pid_t child;
	int listener;
	gui_input_event key = { .type = GUI_INPUT_EVENT_KEY, .key = { .scancode = 0x1e, .pressed = 1 } };
	gui_surface *previous;
	uint64_t previous_frame;

	unlink("/tmp/gui-upload.finish");
	unlink("/tmp/gui-upload.single-paint");
	unlink("/tmp/gui-upload.single-finish");
	hold_upload_replies = 1;
	peer = start_upload_terminal(&server,
		"printf '\\033[?12l'; trap 'printf \"RESIZE-PROGRESS\\n\"' WINCH; "
		"printf '%16384s' '' | tr ' ' X; printf 'PTY-PROGRESS\\n'; "
		"read answer; printf 'KEY-PROGRESS:%s\\n' \"$answer\"; "
		"while test ! -e /tmp/gui-upload.finish; do sleep 1; done",
		"/tmp/gui-upload.term", &listener, &child);
	assert(peer != NULL);
	for (unsigned int attempt = 0; held_upload_bytes != sizeof(held_upload_replies) ||
	     !upload_log_contains("/tmp/gui-upload.term", "PTY-PROGRESS"); attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	window = peer->windows;
	assert(window != NULL && window->committed == NULL && upload_commits == 0);
	{
		struct stat log;

		assert(stat("/tmp/gui-upload.term", &log) == 0 && log.st_size > 16384);
	}
	wm_emit_input(&server, &key);
	key.key.scancode = 0x1c;
	wm_emit_input(&server, &key);
	for (unsigned int attempt = 0; !upload_log_contains("/tmp/gui-upload.term", "KEY-PROGRESS:a"); attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	assert(held_upload_bytes == sizeof(held_upload_replies));
	hold_upload_replies = 0;
	assert(write(peer->fd, held_upload_replies, held_upload_bytes) == (ssize_t)held_upload_bytes);
	for (unsigned int attempt = 0; upload_commits == 0; attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	hold_upload_replies = 1;
	held_upload_bytes = 0;
	for (unsigned int attempt = 0; held_upload_bytes != sizeof(held_upload_replies); attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	assert(upload_commits == 1 && upload_patch_begins == 1 && window->committed != NULL);
	assert(window->committed->pixels[16 * 320 + 16] == 0x00f8fafc);
	assert(wm_resize_window(&server, window, window->x, window->y, 400, 256) == 1);
	for (unsigned int attempt = 0; !upload_log_contains("/tmp/gui-upload.term", "RESIZE-PROGRESS"); attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	hold_upload_replies = 0;
	assert(write(peer->fd, held_upload_replies, held_upload_bytes) == (ssize_t)held_upload_bytes);
	for (unsigned int attempt = 0; upload_commits < 2; attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	assert(upload_commits == 2 && upload_full_begins == 2 && upload_patch_begins == 1);
	assert(window->committed != NULL && window->committed->width == 400 && window->committed->height == 256);
	finish_upload_terminal(&server, peer, listener, child, "/tmp/gui-upload.finish");
	peer = start_upload_terminal(&server,
		"printf '\\033[?12l'; while test ! -e /tmp/gui-upload.single-paint; do sleep 1; done; printf A; "
		"while test ! -e /tmp/gui-upload.single-finish; do sleep 1; done",
		"/tmp/gui-upload.single-log", &listener, &child);
	assert(peer != NULL);
	for (unsigned int attempt = 0; upload_commits == 0; attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	window = peer->windows;
	previous = gui_surface_create(320, 192);
	assert(previous != NULL && window != NULL && window->committed != NULL);
	memcpy(previous->pixels, window->committed->pixels, 320 * 192 * 4);
	upload_wire_bytes = 0;
	previous_frame = begin_drawing_measurement(&server);
	touch_marker("/tmp/gui-upload.single-paint");
	for (unsigned int attempt = 0; upload_commits < 2; attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	measure_drawing = 0;
	assert(upload_patch_begins == 1 && upload_full_begins == 1 && upload_wire_bytes == 1140);
	assert(server.state.frame_number == previous_frame + 1 && measured_work.written_bytes == 1024);
	assert(measured_work.flush_requests == 1 && measured_work.writer_calls == 16);
	assert(measured_work.filled_pixels <= 3 * 256 && measured_work.blitted_pixels <= 256);
	for (unsigned int row = 0; row < 192; row++)
		for (unsigned int column = 0; column < 320; column++)
			if (row < 16 || row >= 32 || column < 16 || column >= 32)
				assert(window->committed->pixels[row * 320 + column] == previous->pixels[row * 320 + column]);
	gui_surface_destroy(previous);
	assert_full_scene(&server);
	finish_upload_terminal(&server, peer, listener, child, "/tmp/gui-upload.single-finish");
	unlink("/tmp/gui-upload.legacy-paint");
	unlink("/tmp/gui-upload.legacy-finish");
	upload_legacy = 1;
	peer = start_upload_terminal(&server,
		"printf '\\033[?12l'; while test ! -e /tmp/gui-upload.legacy-paint; do sleep 1; done; printf A; "
		"while test ! -e /tmp/gui-upload.legacy-finish; do sleep 1; done",
		"/tmp/gui-upload.legacy-log", &listener, &child);
	assert(peer != NULL);
	for (unsigned int attempt = 0; !upload_peer_pump(peer); attempt++) {
		assert(attempt < 20000);
		assert(usleep(1000) == 0);
	}
	{
		struct pollfd ready = { .fd = listener, .events = POLLIN };
		int descriptor;

		assert(poll(&ready, 1, 10000) == 1);
		descriptor = accept(listener, NULL, NULL);
		assert(descriptor >= 0 && fcntl(descriptor, F_SETFL, O_NONBLOCK) == 0);
		peer = wm_add_client(&server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY);
		assert(peer != NULL);
	}
	for (unsigned int attempt = 0; upload_commits == 0; attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	touch_marker("/tmp/gui-upload.legacy-paint");
	for (unsigned int attempt = 0; upload_commits < 2; attempt++) {
		assert(attempt < 20000 && !upload_peer_pump(peer));
		assert(usleep(1000) == 0);
	}
	assert(upload_legacy_rejections == 1 && upload_full_begins == 2 && upload_patch_begins == 0);
	assert(peer->protocol_minor == 0 && peer->capabilities == 0);
	assert_full_scene(&server);
	finish_upload_terminal(&server, peer, listener, child, "/tmp/gui-upload.legacy-finish");
	upload_legacy = 0;
	puts("gui-wm-busy: live uploads service PTY, keys and resize with bounded single-cell traffic");
	puts("gui-wm-busy: rejected negotiation reconnects to legacy full-frame uploads");
}

int main(void)
{
	struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
	struct sigaction reap_children = { .sa_handler = SIG_DFL };

	assert(sigaction(SIGPIPE, &ignore_pipe, NULL) == 0);
	assert(sigaction(SIGCHLD, &reap_children, NULL) == 0);
	test_commit(GUI_FLUSH_BUSY, 1);
	test_commit(GUI_FLUSH_ERROR, 1);
	test_commit(GUI_FLUSH_COMPLETE, 0);
	test_status_readers();
	test_occlusion();
	test_fullscreen();
	test_admission();
	test_transitions();
	test_surface_flow();
	test_peer_surface_capacity();
	test_record_capacity();
	test_large_shell_stream();
	test_shell_keyboard_input();
	test_terminal_recovery();
	test_drawing_counter_clipping();
	test_title_double_click();
	test_cursor_shapes();
	test_drawing_work();
	test_sixteen_window_drawing();
	test_multiwindow_capacity_cycles();
	test_motion_batch();
	test_atomic_patches();
	test_upload_window();
	test_live_upload_progress();
	puts("gui-wm-busy: valid commits survive suspended scanout");
	puts("gui-wm-busy: reservations, shell headroom and transition rollback pass");
	puts("gui-wm-busy: real terminal survives pressure and allocation/flush rollback");
	puts("gui-wm-busy: dynamic records, peer budgets and bounded snapshots pass");
	puts("gui-wm-busy: large snapshots, ordered deltas and stable taskbar pages pass");
	puts("gui-wm-busy: valid shell key input preserves the desktop");
	puts("gui-wm-busy: union occlusion retains pixels and charges without scanout");
	puts("gui-wm-busy: marker readers preserve atomic publication and bounded failure");
	puts("gui-wm-busy: fullscreen transactions preserve modes, focus, geometry and charges");
	return 0;
}
