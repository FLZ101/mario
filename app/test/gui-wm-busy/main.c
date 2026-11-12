#include <assert.h>
#include <time.h>
#include <gui/surface.h>

static gui_flush_result scanout_result;
static unsigned int scanout_calls;
static int fail_surface_allocation;

static gui_surface *test_surface_create(int width, int height)
{
	if (fail_surface_allocation) {
		fail_surface_allocation = 0;
		return NULL;
	}
	return gui_surface_create(width, height);
}

gui_flush_result gui_surface_flush_rect_fd(const gui_surface *surface,
		gui_rect rectangle, size_t stride, int descriptor)
{
	assert(surface != NULL && rectangle.w == surface->width && rectangle.h == surface->height);
	assert(stride == (size_t)surface->width * 4 && descriptor == 123);
	scanout_calls++;
	return scanout_result;
}

#define main wm_program_main
#define gui_surface_create test_surface_create
#include "../../gui/desktop/mario-wm/main.c"
#undef gui_surface_create
#undef main

static void test_commit(gui_flush_result result, int graphics)
{
	static wm_server server;
	wm_client *client = &server.clients[0];
	gui_surface *previous = gui_surface_create(8, 8);
	gui_surface *next = gui_surface_create(8, 8);
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	assert(previous != NULL && next != NULL);
	memset(&server, 0, sizeof(server));
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		server.clients[index].fd = -1;
	server.state = (wm_state) { .framebuffer = 123, .graphics = graphics,
		.width = 8, .height = 8, .scanout_stride = 32,
		.surface_bytes = 512, .frame_number = 7,
		.frame_bytes = 256, .surface_limit = 4096,
		.composite = gui_surface_create(8, 8) };
	assert(server.state.composite != NULL);
	*client = (wm_client) { .fd = 124, .hello = 1,
		.role = GUI_PROTOCOL_ENDPOINT_ORDINARY, .window_id = 1,
		.configure_serial = 1, .width = 8, .height = 8,
		.committed = previous, .committed_bytes = 256,
		.staging = next, .staging_bytes = 256, .staging_pixels = 64,
		.staging_frame_serial = 2, .server = &server,
		.output = gui_protocol_output_create(GUI_PROTOCOL_OUTPUT_LIMIT) };
	assert(client->output != NULL);
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
		assert(client->configure_serial == 2);
		assert(server.state.surface_bytes == 256);
		assert(gui_protocol_output_pending(client->output));
	} else {
		assert(client->committed == next && client->staging == NULL);
		assert(client->staging_frame_serial == 0 && client->staging_pixels == 0);
		assert(server.state.surface_bytes == 256);
		assert(!gui_protocol_output_pending(client->output));
	}
	client_discard_staging(client);
	client_discard_committed(client);
	assert(server.state.surface_bytes == 0);
	gui_protocol_output_destroy(client->output);
	gui_surface_destroy(server.state.composite);
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

static void drain_output(wm_client *client)
{
	captured_bytes = 0;
	assert(gui_protocol_output_flush(client->output, capture_output, NULL) ==
		GUI_PROTOCOL_OUTPUT_EMPTY);
}

static void init_server(wm_server *server, uint32_t width, uint32_t height)
{
	memset(server, 0, sizeof(*server));
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		server->clients[index].fd = -1;
	server->state.width = width;
	server->state.height = height;
	assert(surface_bytes(width, height, &server->state.frame_bytes) == 0);
	server->state.surface_limit = app_gui_surface_budget(server->state.frame_bytes);
}

static wm_client *init_client(wm_server *server, size_t index,
				gui_protocol_endpoint_role role)
{
	wm_client *client = &server->clients[index];

	*client = (wm_client) { .fd = dup(STDOUT_FILENO), .hello = 1, .role = role,
		.server = server, .configure_serial = 1, .window_id = (uint32_t)index + 1,
		.width = 64, .height = 48, .x = 64, .y = 64,
		.output = gui_protocol_output_create(GUI_PROTOCOL_OUTPUT_LIMIT) };
	assert(client->fd >= 0 && client->output != NULL);
	if (server->next_window_id < client->window_id)
		server->next_window_id = client->window_id;
	return client;
}

static void cleanup_server(wm_server *server)
{
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		client_destroy(client);
	}
	if (server->desktop_snapshot != NULL) {
		server->state.surface_bytes -= server->desktop_snapshot_bytes;
		gui_surface_destroy(server->desktop_snapshot);
	}
	assert(server->state.surface_bytes == 0);
	gui_surface_destroy(server->state.composite);
}

static void stage(wm_client *client, uint64_t serial, uint64_t frame)
{
	uint8_t begin[sizeof(gui_protocol_wire_surface_begin)] = { 0 };

	put_u32(begin, client->window_id);
	put_u64(begin + 4, serial);
	put_u64(begin + 12, frame);
	put_u32(begin + 28, client->width);
	put_u32(begin + 32, client->height);
	assert(client_stage_surface(client, begin) == 0);
}

static void commit_staged(wm_client *client)
{
	uint8_t commit[sizeof(gui_protocol_wire_surface_commit)] = { 0 };

	assert(client->staging != NULL);
	client->staging_pixels = (size_t)client->width * client->height;
	put_u32(commit, client->window_id);
	put_u64(commit + 4, client->configure_serial);
	put_u64(commit + 12, client->staging_frame_serial);
	assert(client_commit_surface(client, commit) == 0);
}

static void test_admission(void)
{
	static wm_server server;
	wm_client *ordinary;
	wm_client *shell;
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
	ordinary->window_id = 0;
	client_discard_reservation(ordinary);
	put_u32(create, 99);
	put_u32(create + 4, 64);
	put_u32(create + 8, 48);
	server.state.surface_limit = 2 * frame + 64 * 48 * 4 - 1;
	client_message(ordinary, &header, create, sizeof(create));
	assert(ordinary->window_id == 0 && ordinary->reserved_bytes == 0);
	assert(ordinary->hello == 1 && server.state.surface_bytes == frame);
	drain_output(ordinary);
	assert(get_u16(captured_output + 12) == GUI_PROTOCOL_WINDOW_CREATE);
	assert((int32_t)get_u32(captured_output + 8) == -ENOMEM);
	server.state.surface_limit++;
	client_message(ordinary, &header, create, sizeof(create));
	assert(ordinary->window_id != 0 && ordinary->reserved_bytes == 64 * 48 * 4);
	assert(server.state.surface_bytes == frame + ordinary->reserved_bytes);
	header.type = GUI_PROTOCOL_WINDOW_DESTROY;
	put_u32(create, ordinary->window_id);
	client_message(ordinary, &header, create, 4);
	assert(ordinary->reserved_bytes == 0 && ordinary->window_id == 0);
	header.type = GUI_PROTOCOL_WINDOW_CREATE;
	put_u32(create, 100);
	client_message(ordinary, &header, create, sizeof(create));
	assert(ordinary->reserved_bytes == 64 * 48 * 4);
	fail_surface_allocation = 1;
	stage(ordinary, 1, 1);
	assert(ordinary->window_id != 0 && ordinary->configure_serial == 2);
	assert(ordinary->committed == NULL && ordinary->staging == NULL);
	assert(ordinary->reserved_bytes == 0 && server.state.surface_bytes == frame);
	assert(ordinary->width == 64 && ordinary->height == 48 && ordinary->hello == 1);
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
	wm_client *client;
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
	server.state.surface_limit = server.state.frame_bytes + old_bytes + next_bytes - 1;
	assert(wm_resize_window(&server, client, 32, 48, 96, 64) == -ENOMEM);
	assert(client->configure_serial == 1 && client->x == 64 && client->y == 64);
	assert(client->width == 64 && client->height == 48 && client->committed == old);
	assert(!gui_protocol_output_pending(client->output));
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
	assert(old->pixels[0] == 0x00123456 && client->hello == 1);
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
	assert(client->reserved_bytes == 0 && client->staging == NULL && client->hello == 1);
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

static unsigned int rejected_begins;
static unsigned int completed_frames;
static unsigned int rollback_checks;
static int fail_commit;

static void terminal_message(void *context, const gui_protocol_header *header,
				const void *payload, size_t payload_bytes)
{
	wm_client *client = context;
	gui_surface *previous = client->committed;
	int allocation_failure = fail_surface_allocation;
	int flush_failure = fail_commit && header->type == GUI_PROTOCOL_SURFACE_COMMIT &&
		client->staging != NULL;

	if (flush_failure) {
		client->server->state.graphics = 1;
		scanout_result = GUI_FLUSH_ERROR;
		fail_commit = 0;
	}
	client_message(context, header, payload, payload_bytes);
	if (flush_failure)
		client->server->state.graphics = 0;
	if (header->type == GUI_PROTOCOL_SURFACE_BEGIN && client->staging == NULL)
		rejected_begins++;
	if (header->type == GUI_PROTOCOL_SURFACE_COMMIT && client->committed != previous &&
	    client->committed != NULL)
		completed_frames++;
	if ((allocation_failure && header->type == GUI_PROTOCOL_SURFACE_BEGIN) || flush_failure) {
		assert(client->committed == previous && client->staging == NULL);
		assert(client->reserved_bytes == 0 && client->width == 320 && client->height == 192);
		assert(client->hello == 1 && !client->transition_pending);
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
	assert(client_flush(client) == 0 || client->window_id == 0);
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
	wm_client *client;
	wm_client *pressure;
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
	client = wm_add_client(&server, descriptor, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	assert(client != NULL);
	wait_counter(client, &completed_frames, 1);
	pressure = init_client(&server, 1, GUI_PROTOCOL_ENDPOINT_ORDINARY);
	pressure->committed = gui_surface_create(1024, 768);
	assert(pressure->committed != NULL);
	pressure->committed_bytes = server.state.frame_bytes;
	pressure->width = 1024;
	pressure->height = 768;
	server.state.surface_bytes += pressure->committed_bytes;
	assert(wm_resize_window(&server, pressure, 64, 64, 320, 192) == 1);
	touch_marker("/tmp/gui-wm-terminal.paint");
	wait_counter(client, &rejected_begins, 1);
	assert(kill(child, 0) == 0 && client->committed != NULL);
	assert(wm_resize_window(&server, client, 64, 64, 400, 256) == -ENOMEM);
	client_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, -ENOMEM);
	assert(client_flush(client) == 0);
	client_discard_reservation(pressure);
	wait_counter(client, &completed_frames, completed_frames + 1);
	assert(client->width == 320 && client->height == 192 && kill(child, 0) == 0);
	assert(wm_resize_window(&server, client, 64, 64, 256, 192) == 1);
	fail_surface_allocation = 1;
	assert(client_flush(client) == 0);
	wait_counter(client, &rollback_checks, 1);
	wait_counter(client, &completed_frames, completed_frames + 1);
	assert(client->configure_serial == 3 && kill(child, 0) == 0);
	assert(wm_resize_window(&server, client, 64, 64, 256, 192) == 1);
	fail_commit = 1;
	assert(client_flush(client) == 0);
	wait_counter(client, &rollback_checks, 2);
	wait_counter(client, &completed_frames, completed_frames + 1);
	assert(client->configure_serial == 5 && kill(child, 0) == 0);
	assert(server.state.surface_bytes == server.state.frame_bytes + terminal_bytes);
	touch_marker("/tmp/gui-wm-terminal.finish");
	for (unsigned int attempt = 0;; attempt++) {
		pid_t result = waitpid(child, &status, WNOHANG);

		assert(result >= 0 && attempt < 2000);
		if (result == child)
			break;
		pump_terminal(client);
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

int main(void)
{
	struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
	struct sigaction reap_children = { .sa_handler = SIG_DFL };

	assert(sigaction(SIGPIPE, &ignore_pipe, NULL) == 0);
	assert(sigaction(SIGCHLD, &reap_children, NULL) == 0);
	test_commit(GUI_FLUSH_BUSY, 1);
	test_commit(GUI_FLUSH_ERROR, 1);
	test_commit(GUI_FLUSH_COMPLETE, 0);
	test_admission();
	test_transitions();
	test_surface_flow();
	test_terminal_recovery();
	puts("gui-wm-busy: valid commits survive suspended scanout");
	puts("gui-wm-busy: reservations, shell headroom and transition rollback pass");
	puts("gui-wm-busy: real terminal survives pressure and allocation/flush rollback");
	return 0;
}
