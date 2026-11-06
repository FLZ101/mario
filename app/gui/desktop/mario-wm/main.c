#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <mario/fb.h>
#include <mario/input.h>
#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define WM_STARTED_PATH "/tmp/mario-wm.started"
#define WM_READY_PATH "/tmp/mario-wm.ready"
#define WM_FRAME_PATH "/tmp/mario-wm.frame"
#define WM_CLIENT_LIMIT 8
#define WM_SHELL_PATH "/usr/bin/mario-desktop"
#define WM_SHELL_FD 3
#define WM_CURSOR_PATH "/usr/share/mario/cursor-arrow-16.rgba"
#define WM_CURSOR_WIDTH 16U
#define WM_CURSOR_HEIGHT 16U

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void remove_status_files(void)
{
	/* These are server-owned diagnostics, never client-controlled state. */
	unlink(WM_SOCKET_PATH);
	unlink(WM_STARTED_PATH);
	unlink(WM_READY_PATH);
	unlink(WM_FRAME_PATH);
	unlink(WM_READY_PATH ".new");
	unlink(WM_FRAME_PATH ".new");
}

typedef struct {
	int framebuffer;
	int gui;
	int mouse;
	int listener;
	int owns_status;
	int graphics;
	uint32_t width;
	uint32_t height;
	size_t frame_bytes;
	size_t scanout_stride;
	size_t surface_limit;
	size_t surface_bytes;
	uint64_t frame_number;
	uint32_t cursor_x;
	uint32_t cursor_y;
	uint8_t cursor_rgba[WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT * 4];
	gui_surface *composite;
} wm_state;

typedef struct wm_server wm_server;

typedef struct wm_client {
	int fd;
	int hello;
	int focused;
	gui_protocol_endpoint_role role;
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
	uint64_t configure_serial;
	uint64_t staging_frame_serial;
	size_t staging_bytes;
	size_t staging_pixels;
	gui_surface *staging;
	size_t committed_bytes;
	gui_surface *committed;
	gui_protocol_input *input;
	gui_protocol_output *output;
	wm_server *server;
} wm_client;

struct wm_server {
	wm_state state;
	wm_client clients[WM_CLIENT_LIMIT];
	/* All focus and future grab transitions enter through this owner. */
	wm_client *input_owner;
};

static int write_status_file(const char *path, const char *temporary_path,
			     const char *contents, size_t byte_count)
{
	int fd = open(temporary_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0)
		return -1;
	while (byte_count != 0) {
		ssize_t count = write(fd, contents, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			int saved_errno = count == 0 ? EIO : errno;

			close(fd);
			unlink(temporary_path);
			errno = saved_errno;
			return -1;
		}
		contents += count;
		byte_count -= count;
	}
	if (close(fd) < 0) {
		int saved_errno = errno;

		unlink(temporary_path);
		errno = saved_errno;
		return -1;
	}
	if (rename(temporary_path, path) < 0) {
		int saved_errno = errno;

		unlink(temporary_path);
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int wm_publish_completed_frame(wm_state *state)
{
	char marker[32];
	uint64_t frame_number = state->frame_number + 1;
	int length = snprintf(marker, sizeof(marker), "%llu\n",
		(unsigned long long)frame_number);

	if (length < 0 || (size_t)length >= sizeof(marker)) {
		errno = EOVERFLOW;
		return -1;
	}
	/* Rename keeps a reader from observing a truncated counter during an update. */
	if (write_status_file(WM_FRAME_PATH, WM_FRAME_PATH ".new", marker,
			      (size_t)length) < 0)
		return -1;
	state->frame_number = frame_number;
	/* Readiness is published only after the corresponding frame is visible. */
	return write_status_file(WM_READY_PATH, WM_READY_PATH ".new", "ready\n", 6);
}

static uint16_t get_u16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static uint32_t get_u32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
	       (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t get_u64(const uint8_t *bytes)
{
	uint64_t value = 0;

	for (unsigned int index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
	for (unsigned int index = 0; index < 8; index++)
		bytes[index] = value >> (index * 8);
}

static void client_discard_staging(wm_client *client)
{
	if (client->staging != NULL) {
		client->server->state.surface_bytes -= client->staging_bytes;
		gui_surface_destroy(client->staging);
		client->staging = NULL;
		client->staging_bytes = 0;
		client->staging_frame_serial = 0;
	}
	client->staging_pixels = 0;
}

static void client_discard_committed(wm_client *client)
{
	if (client->committed != NULL) {
		client->server->state.surface_bytes -= client->committed_bytes;
		gui_surface_destroy(client->committed);
		client->committed = NULL;
		client->committed_bytes = 0;
	}
}

static void client_destroy(wm_client *client)
{
	if (client->fd >= 0)
		close(client->fd);
	client_discard_staging(client);
	client_discard_committed(client);
	gui_protocol_input_destroy(client->input);
	gui_protocol_output_destroy(client->output);
	*client = (wm_client) { .fd = -1 };
}

static int wm_load_cursor(wm_state *state)
{
	uint8_t *bytes = state->cursor_rgba;
	uint8_t extra;
	size_t remaining = sizeof(state->cursor_rgba);
	int fd = open(WM_CURSOR_PATH, O_RDONLY);

	if (fd < 0)
		return -1;
	while (remaining != 0) {
		ssize_t count = read(fd, bytes, remaining);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			int saved_errno = count == 0 ? EIO : errno;

			close(fd);
			errno = saved_errno;
			return -1;
		}
		bytes += count;
		remaining -= count;
	}
	if (read(fd, &extra, 1) != 0) {
		int saved_errno = errno == 0 ? EIO : errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	if (close(fd) < 0)
		return -1;
	for (size_t index = 3; index < sizeof(state->cursor_rgba); index += 4) {
		if (state->cursor_rgba[index] != 0 && state->cursor_rgba[index] != 255) {
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

static void wm_draw_cursor(wm_state *state)
{
	for (uint32_t row = 0; row < WM_CURSOR_HEIGHT; row++) {
		uint32_t y = state->cursor_y + row;

		if (y >= state->height)
			break;
		for (uint32_t column = 0; column < WM_CURSOR_WIDTH; column++) {
			uint32_t x = state->cursor_x + column;
			size_t source = ((size_t)row * WM_CURSOR_WIDTH + column) * 4;

			if (x >= state->width)
				break;
			if (state->cursor_rgba[source + 3] != 0) {
				state->composite->pixels[(size_t)y * state->composite->stride + x] =
					(uint32_t)state->cursor_rgba[source] << 16 |
					(uint32_t)state->cursor_rgba[source + 1] << 8 |
					state->cursor_rgba[source + 2];
			}
		}
	}
}

static gui_flush_result wm_compose(wm_server *server)
{
	wm_state *state = &server->state;
	gui_surface *desktop = NULL;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		if (client->fd >= 0 && client->role == GUI_PROTOCOL_ENDPOINT_SHELL &&
		    client->committed != NULL)
			desktop = client->committed;
	}
	if (!state->graphics)
		return GUI_FLUSH_COMPLETE;
	gui_fill_rect(state->composite, (gui_rect) { .x = 0, .y = 0,
		.w = state->composite->width, .h = state->composite->height }, 0);
	if (desktop != NULL) {
		gui_blit(state->composite, 0, 0, desktop,
			&(gui_rect) { .x = 0, .y = 0, .w = desktop->width,
				.h = desktop->height });
	}
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		if (client->fd >= 0 && client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
		    client->committed != NULL) {
			gui_blit(state->composite, 0, 0, client->committed,
				&(gui_rect) { .x = 0, .y = 0,
					.w = client->committed->width,
					.h = client->committed->height });
		}
	}
	/* The server-owned sprite is always the final layer, independent of focus. */
	wm_draw_cursor(state);
	{
		gui_flush_result result = gui_surface_flush_rect_fd(state->composite,
			(gui_rect) { .x = 0, .y = 0, .w = state->composite->width,
				.h = state->composite->height }, state->scanout_stride,
			state->framebuffer);

		if (result != GUI_FLUSH_COMPLETE)
			return result;
	}
	return wm_publish_completed_frame(state) == 0 ? GUI_FLUSH_COMPLETE :
		GUI_FLUSH_ERROR;
}

static wm_client *wm_add_client(wm_server *server, int fd,
				gui_protocol_endpoint_role role)
{
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		if (client->fd >= 0)
			continue;
		*client = (wm_client) { .fd = fd, .role = role,
			.input = gui_protocol_input_create(),
			.output = gui_protocol_output_create(GUI_PROTOCOL_OUTPUT_LIMIT),
			.server = server };
		if (client->input == NULL || client->output == NULL) {
			client_destroy(client);
			return NULL;
		}
		return client;
	}
	close(fd);
	return NULL;
}

static int surface_bytes(uint32_t width, uint32_t height, size_t *bytes)
{
	size_t pixels;

	if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX)
		return -1;
	pixels = width;
	if (pixels > SIZE_MAX / height || pixels * height > SIZE_MAX / sizeof(gui_color))
		return -1;
	*bytes = pixels * height * sizeof(gui_color);
	return 0;
}

static int wm_init_surface_budget(wm_state *state)
{
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo info;
	size_t line_bytes;

	if (ioctl(state->framebuffer, FBIOGET_VSCREENINFO, &info) < 0 ||
	    ioctl(state->framebuffer, FBIOGET_FSCREENINFO, &fix) < 0 ||
	    surface_bytes(info.xres, info.yres, &state->frame_bytes) < 0 ||
	    surface_bytes(info.xres, 1, &line_bytes) < 0 ||
	    fix.line_length < line_bytes || state->frame_bytes > SIZE_MAX / 2 ||
	    state->frame_bytes * 2 > SIZE_MAX - state->frame_bytes / 2)
		return -1;
	state->width = info.xres;
	state->height = info.yres;
	state->scanout_stride = fix.line_length;
	/* One full frame may be committed while its full replacement is staged. */
	state->surface_limit = state->frame_bytes * 2 + state->frame_bytes / 2;
	return 0;
}

static int client_enqueue(wm_client *client, gui_protocol_type type,
			  const void *payload, size_t payload_bytes)
{
	uint8_t *frame;
	uint32_t length;
	int result;

	if (gui_protocol_message_length(payload_bytes, &length) < 0 ||
	    (frame = malloc(length)) == NULL)
		return -1;
	if (gui_protocol_encode_header(frame, length, type, payload_bytes) < 0) {
		free(frame);
		return -1;
}
	if (payload_bytes != 0)
		memcpy(frame + GUI_PROTOCOL_HEADER_SIZE, payload, payload_bytes);
	result = gui_protocol_output_enqueue(client->output, frame, length);
	free(frame);
	return result;
}

static void client_reject(wm_client *client, gui_protocol_type failed_type,
			  int status)
{
	uint8_t error[sizeof(gui_protocol_wire_error)];

	put_u32(error, (uint32_t)status);
	put_u16(error + 4, failed_type);
	put_u16(error + 6, 0);
	if (client_enqueue(client, GUI_PROTOCOL_ERROR, error, sizeof(error)) < 0)
		client->hello = -1;
}

static int client_stage_surface(wm_client *client, const uint8_t *bytes)
{
	wm_state *state = &client->server->state;
	uint32_t window_id = get_u32(bytes);
	uint64_t configure_serial = get_u64(bytes + 4);
	uint64_t frame_serial = get_u64(bytes + 12);
	int32_t x = (int32_t)get_u32(bytes + 20);
	int32_t y = (int32_t)get_u32(bytes + 24);
	uint32_t width = get_u32(bytes + 28);
	uint32_t height = get_u32(bytes + 32);
	size_t bytes_needed;
	gui_surface *surface;

	if (window_id != client->window_id ||
	    configure_serial != client->configure_serial || frame_serial == 0 ||
	    x != 0 || y != 0 || width != client->width || height != client->height ||
	    surface_bytes(width, height, &bytes_needed) < 0 ||
	    bytes_needed > state->frame_bytes) {
		client_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -EINVAL);
		return 0;
	}
	if (client->staging != NULL) {
		client_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -EBUSY);
		return 0;
	}
	if (bytes_needed > state->surface_limit - state->surface_bytes) {
		client_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -ENOMEM);
		return 0;
	}
	surface = gui_surface_create((int)width, (int)height);
	if (surface == NULL) {
		client_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -ENOMEM);
		return 0;
	}
	client->staging = surface;
	client->staging_bytes = bytes_needed;
	client->staging_frame_serial = frame_serial;
	client->staging_pixels = 0;
	state->surface_bytes += bytes_needed;
	return 0;
}

static int client_surface_data(wm_client *client, const uint8_t *bytes,
			       size_t payload_bytes)
{
	uint32_t window_id = get_u32(bytes);
	uint64_t configure_serial = get_u64(bytes + 4);
	uint64_t frame_serial = get_u64(bytes + 12);
	uint32_t pixel_offset = get_u32(bytes + 20);
	uint32_t pixel_count = get_u32(bytes + 24);
	size_t surface_pixels;
	size_t byte_count;

	if (client->staging == NULL || window_id != client->window_id ||
	    configure_serial != client->configure_serial ||
	    frame_serial != client->staging_frame_serial || pixel_count == 0 ||
	    client->width > SIZE_MAX / client->height ||
	    (surface_pixels = (size_t)client->width * client->height) > UINT32_MAX ||
	    pixel_offset != client->staging_pixels || pixel_offset > surface_pixels ||
	    pixel_count > surface_pixels - pixel_offset ||
	    pixel_count > SIZE_MAX / sizeof(gui_color) ||
	    (byte_count = (size_t)pixel_count * sizeof(gui_color)) !=
		payload_bytes - sizeof(gui_protocol_wire_surface_data)) {
		client_reject(client, GUI_PROTOCOL_SURFACE_DATA, -EINVAL);
		return 0;
	}
	memcpy(client->staging->pixels + pixel_offset,
	       bytes + sizeof(gui_protocol_wire_surface_data), byte_count);
	client->staging_pixels += pixel_count;
	return client_enqueue(client, GUI_PROTOCOL_SURFACE_DATA_REPLY, bytes,
			      sizeof(gui_protocol_wire_surface_data));
}

static int client_commit_surface(wm_client *client, const uint8_t *bytes)
{
	uint32_t window_id = get_u32(bytes);
	uint64_t configure_serial = get_u64(bytes + 4);
	uint64_t frame_serial = get_u64(bytes + 12);
	gui_flush_result result;

	if (client->staging == NULL || window_id != client->window_id ||
	    configure_serial != client->configure_serial ||
	    frame_serial != client->staging_frame_serial ||
	    client->staging_pixels != (size_t)client->width * client->height) {
		client_reject(client, GUI_PROTOCOL_SURFACE_COMMIT, -EINVAL);
		return 0;
	}
	{
		gui_surface *old = client->committed;
		size_t old_bytes = client->committed_bytes;

		client->committed = client->staging;
		client->committed_bytes = client->staging_bytes;
		client->staging = NULL;
		client->staging_bytes = 0;
		result = wm_compose(client->server);
		if (result != GUI_FLUSH_COMPLETE) {
			client->staging = client->committed;
			client->staging_bytes = client->committed_bytes;
			client->committed = old;
			client->committed_bytes = old_bytes;
			client_reject(client, GUI_PROTOCOL_SURFACE_COMMIT,
				      result == GUI_FLUSH_BUSY ? -EBUSY : -EIO);
			return 0;
		}
		if (old != NULL) {
			client->server->state.surface_bytes -= old_bytes;
			gui_surface_destroy(old);
		}
	}
	client->staging_frame_serial = 0;
	client->staging_pixels = 0;
	return 0;
}

static ssize_t client_write(void *context, const void *bytes, size_t byte_count)
{
	wm_client *client = context;

	return write(client->fd, bytes, byte_count);
}

static int client_flush(wm_client *client)
{
	return gui_protocol_output_flush(client->output, client_write, client) !=
	       GUI_PROTOCOL_OUTPUT_ERROR ? 0 : -1;
}

static int client_enqueue_focus(wm_client *client, int focused)
{
	uint8_t focus[sizeof(gui_protocol_wire_window_focus)];

	put_u32(focus, client->window_id);
	put_u32(focus + 4, focused);
	return client_enqueue(client, GUI_PROTOCOL_WINDOW_FOCUS, focus, sizeof(focus));
}

static int client_enqueue_input_reset(wm_client *client)
{
	return client_enqueue(client, GUI_PROTOCOL_INPUT_RESET, NULL, 0);
}

static void wm_reset_input_owner(wm_server *server)
{
	wm_client *client = server->input_owner;

	if (client != NULL && client_enqueue_input_reset(client) < 0)
		client->hello = -1;
}

static void wm_set_input_owner(wm_server *server, wm_client *client)
{
	wm_client *previous = server->input_owner;

	if (previous == client)
		return;
	/*
	 * Reset both sides of a transfer.  The old client drops stuck state before
	 * losing ownership, while the new client cannot inherit a key or button
	 * that was physically pressed before it became the recipient.  Future
	 * pointer-grab transitions use this same ordered boundary.
	 */
	if (previous != NULL) {
		previous->focused = 0;
		if (client_enqueue_focus(previous, 0) < 0 ||
		    client_enqueue_input_reset(previous) < 0)
			previous->hello = -1;
	}
	server->input_owner = client;
	if (client != NULL) {
		client->focused = 1;
		if (client_enqueue_focus(client, 1) < 0 ||
		    client_enqueue_input_reset(client) < 0)
			client->hello = -1;
	}
}

static wm_client *wm_next_focus(wm_server *server)
{
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		if (client->fd >= 0 && client->hello > 0 &&
		    client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
		    client->window_id != 0)
			return client;
	}
	return NULL;
}

static void wm_disconnect_client(wm_server *server, wm_client *client)
{
	if (client->role == GUI_PROTOCOL_ENDPOINT_SHELL) {
		/* M3.4 has no shell-relaunch policy, so a shell loss ends the session. */
		client_destroy(client);
		stop_requested = 1;
		return;
	}
	if (server->input_owner == client) {
		/* The peer is gone, but clearing ownership prevents state leakage. */
		wm_set_input_owner(server, NULL);
		client_destroy(client);
		wm_set_input_owner(server, wm_next_focus(server));
		return;
	}
	client_destroy(client);
}

static void wm_emit_input(void *context, const gui_input_event *event)
{
	wm_server *server = context;
	wm_client *client = server->input_owner;
	uint8_t message[sizeof(gui_protocol_wire_input_event)] = { 0 };

	if (event->type == GUI_INPUT_EVENT_KEY_RESET ||
	    event->type == GUI_INPUT_EVENT_MOUSE_RESET) {
		wm_reset_input_owner(server);
		return;
	}
	if (!server->state.graphics)
		return;
	if (event->type == GUI_INPUT_EVENT_MOUSE) {
		wm_state *state = &server->state;
		int64_t x = (int64_t)state->cursor_x + event->mouse.dx;
		int64_t y = (int64_t)state->cursor_y - event->mouse.dy;

		if (x < 0)
			x = 0;
		else if ((uint64_t)x >= state->width)
			x = state->width - 1;
		if (y < 0)
			y = 0;
		else if ((uint64_t)y >= state->height)
			y = state->height - 1;
		if (state->cursor_x != (uint32_t)x || state->cursor_y != (uint32_t)y) {
			state->cursor_x = x;
			state->cursor_y = y;
			if (wm_compose(server) == GUI_FLUSH_ERROR)
				stop_requested = 1;
		}
	}
	if (client == NULL)
		return;
	put_u32(message, client->window_id);
	put_u32(message + 4, event->type);
	if (event->type == GUI_INPUT_EVENT_KEY)
		put_u32(message + 8, event->key.scancode);
	put_u32(message + 24, event->ticks);
	if (client_enqueue(client, GUI_PROTOCOL_INPUT_EVENT, message,
			   sizeof(message)) < 0)
		client->hello = -1;
}

static void client_message(void *context, const gui_protocol_header *header,
			   const void *payload, size_t payload_bytes)
{
	wm_client *client = context;
	const uint8_t *bytes = payload;
	uint8_t reply[sizeof(gui_protocol_wire_window_create_reply)];

	if (!client->hello) {
		if (header->type != GUI_PROTOCOL_HELLO || payload_bytes != 4 ||
		    get_u16(bytes) != GUI_PROTOCOL_VERSION_MAJOR ||
		    get_u16(bytes + 2) > GUI_PROTOCOL_VERSION_MINOR) {
			client->hello = -1;
			return;
		}
		put_u16(reply, GUI_PROTOCOL_VERSION_MAJOR);
		put_u16(reply + 2, GUI_PROTOCOL_VERSION_MINOR);
		client->hello = client_enqueue(client, GUI_PROTOCOL_HELLO_REPLY, reply, 4) == 0;
		if (!client->hello)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CREATE && client->window_id == 0 &&
	    payload_bytes >= sizeof(gui_protocol_wire_window_create) &&
	    payload_bytes == sizeof(gui_protocol_wire_window_create) + get_u16(bytes + 12) &&
	    get_u32(bytes + 4) != 0 && get_u32(bytes + 8) != 0) {
		client->window_id = 1;
		client->width = get_u32(bytes + 4);
		client->height = get_u32(bytes + 8);
		client->configure_serial = 1;
		put_u32(reply, get_u32(bytes));
		put_u32(reply + 4, client->window_id);
		put_u64(reply + 8, 1);
		put_u32(reply + 16, get_u32(bytes + 4));
		put_u32(reply + 20, get_u32(bytes + 8));
		if (client_enqueue(client, GUI_PROTOCOL_WINDOW_CREATE_REPLY, reply,
				   sizeof(reply)) < 0)
			client->hello = -1;
		else
			wm_set_input_owner(client->server, client);
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_BEGIN &&
	    payload_bytes == sizeof(gui_protocol_wire_surface_begin)) {
		if (client_stage_surface(client, bytes) < 0)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_DATA &&
	    payload_bytes >= sizeof(gui_protocol_wire_surface_data)) {
		if (client_surface_data(client, bytes, payload_bytes) < 0)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_COMMIT &&
	    payload_bytes == sizeof(gui_protocol_wire_surface_commit)) {
		if (client_commit_surface(client, bytes) < 0)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_DESKTOP_CREATE &&
	    payload_bytes == sizeof(gui_protocol_wire_desktop_create)) {
		if (client->role != GUI_PROTOCOL_ENDPOINT_SHELL)
			client_reject(client, GUI_PROTOCOL_DESKTOP_CREATE, -EPERM);
		else if (client->window_id != 0)
			client_reject(client, GUI_PROTOCOL_DESKTOP_CREATE, -EBUSY);
		else {
			client->window_id = 1;
			client->width = client->server->state.width;
			client->height = client->server->state.height;
			client->configure_serial = 1;
			put_u32(reply, get_u32(bytes));
			put_u32(reply + 4, client->window_id);
			put_u64(reply + 8, client->configure_serial);
			put_u32(reply + 16, client->width);
			put_u32(reply + 20, client->height);
			if (client_enqueue(client, GUI_PROTOCOL_DESKTOP_CREATE_REPLY, reply,
				   sizeof(reply)) < 0)
				client->hello = -1;
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_DESTROY && payload_bytes == 4 &&
	    get_u32(bytes) == client->window_id) {
		if (client->server->input_owner == client)
			wm_set_input_owner(client->server, NULL);
		client_discard_staging(client);
		client_discard_committed(client);
		client->window_id = 0;
		return;
	}
	client->hello = -1;
}

static void wm_cleanup(wm_state *state)
{
	/* Close the listener before releasing the descriptors that own the seat. */
	if (state->listener >= 0)
		close(state->listener);
	if (state->gui >= 0)
		close(state->gui);
	if (state->mouse >= 0)
		close(state->mouse);
	if (state->framebuffer >= 0)
		close(state->framebuffer);
	gui_surface_destroy(state->composite);
	if (state->owns_status)
		remove_status_files();
}

static int create_started_marker(void)
{
	static const char marker[] = "listening\n";
	int fd = open(WM_STARTED_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);

	if (fd < 0)
		return -1;
	if (write(fd, marker, sizeof(marker) - 1) != sizeof(marker) - 1) {
		int saved_errno = errno;

		close(fd);
		errno = saved_errno;
		return -1;
	}
	return close(fd);
}

static int bind_listener(void)
{
	struct sockaddr_un address;
	int listener;

	_Static_assert(sizeof(WM_SOCKET_PATH) <= sizeof(address.sun_path),
		       "window manager socket path fits sockaddr_un");
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (listener < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, WM_SOCKET_PATH, sizeof(WM_SOCKET_PATH));
	if (bind(listener, (const struct sockaddr *)&address,
		 sizeof(address.sun_family) + sizeof(WM_SOCKET_PATH)) < 0 ||
	    listen(listener, 16) < 0) {
		int saved_errno = errno;

		close(listener);
		errno = saved_errno;
		return -1;
	}
	return listener;
}

static void shell_exec(int fd)
{
	if (fd != WM_SHELL_FD) {
		if (dup2(fd, WM_SHELL_FD) < 0)
			_exit(127);
		close(fd);
	}
	/* Standard streams are diagnostics; fd 3 is the shell's only capability. */
	for (int inherited_fd = WM_SHELL_FD + 1; inherited_fd < 1024;
	     inherited_fd++)
		close(inherited_fd);
	execl(WM_SHELL_PATH, WM_SHELL_PATH, "--wm-fd=3", NULL);
	_exit(127);
}

static int wm_start_shell(wm_server *server)
{
	int fds[2];
	int flags;
	pid_t pid;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
		return -1;
	flags = fcntl(fds[0], F_GETFL);
	if (flags < 0 || fcntl(fds[0], F_SETFL, flags | O_NONBLOCK) < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		close(fds[0]);
		shell_exec(fds[1]);
	}
	close(fds[1]);
	if (wm_add_client(server, fds[0], GUI_PROTOCOL_ENDPOINT_SHELL) == NULL)
		return -1;
	return 0;
}

int main(void)
{
	struct sigaction action = { .sa_handler = request_stop };
	struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
	struct pollfd pollfds[WM_CLIENT_LIMIT + 4];
	struct mario_gui_key_event events[16];
	struct mario_psaux_event mouse_events[16];
	wm_server server = { .state = { .framebuffer = -1, .gui = -1,
		.mouse = -1, .listener = -1 } };
	int status = 1;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		server.clients[index].fd = -1;

	/* Acquire both descriptors before F8 may expose the graphics seat. */
	server.state.framebuffer = open("/dev/fb0", O_RDWR);
	if (server.state.framebuffer < 0)
		goto out;
	if (wm_init_surface_budget(&server.state) < 0)
		goto out;
	server.state.composite = gui_surface_create(server.state.width, server.state.height);
	if (server.state.composite == NULL || wm_load_cursor(&server.state) < 0)
		goto out;
	/* Keep the initial sprite clear of the origin used by desktop-frame checks. */
	server.state.cursor_x = server.state.width > WM_CURSOR_WIDTH ? WM_CURSOR_WIDTH : 0;
	server.state.cursor_y = server.state.height > WM_CURSOR_HEIGHT ? WM_CURSOR_HEIGHT : 0;
	server.state.gui = open("/dev/gui", O_RDONLY | O_NONBLOCK);
	if (server.state.gui < 0)
		goto out;
	/* The seat owner reads both input streams so the cursor cannot be spoofed. */
	server.state.mouse = open("/dev/psaux", O_RDONLY | O_NONBLOCK);
	if (server.state.mouse < 0)
		goto out;
	server.state.owns_status = 1;
	/* Only the exclusive owner may discard a predecessor's stale status. */
	remove_status_files();
	server.state.listener = bind_listener();
	if (server.state.listener < 0)
		goto out;
	printf("mario-wm: listening\n");
	fflush(stdout);
	/* The public listener is ready before the private shell may start. */
	if (wm_start_shell(&server) < 0)
		goto out;
	if (create_started_marker() < 0)
		goto out;
	/* Socket write failures are handled per-client by the output queue. */
	if (sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGPIPE, &ignore_pipe, NULL) < 0)
		goto out;
	while (!stop_requested) {
		int ready;
		size_t poll_count = 4;

		pollfds[0] = (struct pollfd) { .fd = server.state.gui, .events = POLLIN };
		pollfds[1] = (struct pollfd) { .fd = server.state.framebuffer,
			.events = POLLPRI };
		pollfds[2] = (struct pollfd) { .fd = server.state.listener, .events = POLLIN };
		pollfds[3] = (struct pollfd) { .fd = server.state.mouse, .events = POLLIN };
		for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
			short events = POLLIN;

			if (server.clients[index].fd < 0)
				continue;
			/* A socket stays writable while idle, so polling POLLOUT would spin. */
			if (gui_protocol_output_pending(server.clients[index].output))
				events |= POLLOUT;
			pollfds[poll_count++] = (struct pollfd) {
				.fd = server.clients[index].fd,
				.events = events,
			};
		}
		ready = poll(pollfds, poll_count, -1);

		if (ready < 0) {
			if (errno == EINTR)
				continue;
			goto out;
		}
		if (pollfds[1].revents & POLLPRI) {
			uint32_t graphics;

			if (ioctl(server.state.framebuffer, FBIOGET_CONSOLE_STATE,
				  &graphics) < 0)
				goto out;
			server.state.graphics = graphics == FB_CONSOLE_GRAPHICS;
			if (!server.state.graphics) {
				/* A stale completed frame is never a readiness signal in text mode. */
				unlink(WM_READY_PATH);
			} else {
				gui_flush_result result = wm_compose(&server);

				if (result == GUI_FLUSH_ERROR)
					goto out;
				/* EBUSY leaves the prior frame counter and readiness untouched. */
			}
			/* A console switch invalidates every physical pressed-state bit. */
			wm_reset_input_owner(&server);
		}
		if (pollfds[0].revents & POLLIN) {
			ssize_t event_bytes;

			while ((event_bytes = read(server.state.gui, events, sizeof(events))) > 0) {
				gui_input_normalize_key_records(events,
						event_bytes / sizeof(events[0]),
						wm_emit_input, &server);
			}
			if (errno != EAGAIN)
				goto out;
		}
		if (pollfds[3].revents & POLLIN) {
			ssize_t event_bytes;

			while ((event_bytes = read(server.state.mouse, mouse_events,
					       sizeof(mouse_events))) > 0) {
				gui_input_normalize_mouse_records(mouse_events,
					event_bytes / sizeof(mouse_events[0]), wm_emit_input, &server);
			}
			if (errno != EAGAIN)
				goto out;
		}
		for (size_t index = 0, poll_index = 4; index < WM_CLIENT_LIMIT; index++) {
			uint8_t bytes[4096];
			ssize_t count = -1;

			if (server.clients[index].fd < 0)
				continue;
			if (pollfds[poll_index].revents & (POLLIN | POLLHUP | POLLERR)) {
				while ((count = recv(server.clients[index].fd, bytes, sizeof(bytes), 0)) > 0) {
					if (gui_protocol_input_append(server.clients[index].input, bytes, count) < 0 ||
					    gui_protocol_input_drain(server.clients[index].input, client_message,
							     &server.clients[index]) < 0 ||
					    client_flush(&server.clients[index]) < 0)
					server.clients[index].hello = -1;
				}
				if (count == 0 ||
				    (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
					server.clients[index].hello = -1;
			}
			if (server.clients[index].fd >= 0 &&
			    (pollfds[poll_index].revents & POLLOUT) &&
			    client_flush(&server.clients[index]) < 0)
				server.clients[index].hello = -1;
			/* A socket hangup is terminal even if a nonblocking recv() saw EAGAIN. */
			if (pollfds[poll_index].revents & (POLLHUP | POLLERR | POLLNVAL))
				server.clients[index].hello = -1;
			if (server.clients[index].hello < 0)
				wm_disconnect_client(&server, &server.clients[index]);
			poll_index++;
		}
		if (pollfds[2].revents & POLLIN) {
			int fd = accept4(server.state.listener, NULL, NULL, SOCK_NONBLOCK);

			if (fd >= 0) {
				/* A public peer can never acquire the private shell role. */
				(void)wm_add_client(&server, fd,
					GUI_PROTOCOL_ENDPOINT_ORDINARY);
			}
		}
	}
	status = 0;
out:
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++)
		client_destroy(&server.clients[index]);
	if (status != 0)
		perror("mario-wm");
	wm_cleanup(&server.state);
	return status;
}
