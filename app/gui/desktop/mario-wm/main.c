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
#define WM_TERMINAL_PATH "/usr/bin/terminal"
#define WM_CURSOR_PATH "/usr/share/mario/cursor-arrow-16.rgba"
#define WM_CURSOR_WIDTH 16U
#define WM_CURSOR_HEIGHT 16U
#define WM_WINDOW_BORDER 1U
#define WM_TITLE_HEIGHT 24U
#define WM_CLOSE_BUTTON_SIZE 24U
#define WM_MIN_CONTENT_WIDTH 64U
#define WM_MIN_CONTENT_HEIGHT 48U
#define WM_WINDOW_BORDER_COLOR 0x000f172aU
#define WM_WINDOW_TITLE_ACTIVE_COLOR 0x002563ebU
#define WM_WINDOW_TITLE_INACTIVE_COLOR 0x00475569U
#define WM_WINDOW_CONTENT_COLOR 0x00f8fafcU

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
	uint32_t modifiers;
	uint8_t mouse_buttons;
	uint8_t key_down[512];
	uint8_t cursor_rgba[WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT * 4];
	gui_surface *composite;
} wm_state;

typedef struct wm_server wm_server;

typedef enum {
	WM_CAPTURE_NONE,
	WM_CAPTURE_MOVE,
	WM_CAPTURE_RESIZE_LEFT,
	WM_CAPTURE_RESIZE_RIGHT,
	WM_CAPTURE_RESIZE_TOP,
	WM_CAPTURE_RESIZE_BOTTOM,
	WM_CAPTURE_RESIZE_TOP_LEFT,
	WM_CAPTURE_RESIZE_TOP_RIGHT,
	WM_CAPTURE_RESIZE_BOTTOM_LEFT,
	WM_CAPTURE_RESIZE_BOTTOM_RIGHT,
} wm_capture_operation;

typedef struct wm_client {
	int fd;
	int hello;
	int focused;
	gui_protocol_endpoint_role role;
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
	int32_t x;
	int32_t y;
	uint64_t stack_order;
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
	gui_surface *desktop_snapshot;
	size_t desktop_snapshot_bytes;
	uint32_t next_window_id;
	uint64_t next_placement;
	uint64_t next_stack_order;
	/* All focus and future grab transitions enter through this owner. */
	wm_client *input_owner;
	/* Pointer capture keeps title and border drags out of client input streams. */
	wm_client *capture_client;
	wm_capture_operation capture_operation;
	uint32_t capture_x;
	uint32_t capture_y;
	int32_t capture_window_x;
	int32_t capture_window_y;
	uint32_t capture_width;
	uint32_t capture_height;
	int shell_ready;
	int shell_replacement_started;
};

static int wm_start_shell(wm_server *server);
static int wm_start_terminal(void);

static uint32_t wm_allocate_window_id(wm_server *server)
{
	if (server->next_window_id == UINT32_MAX)
		return 0;
	return ++server->next_window_id;
}

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

static int mark_close_on_exec(int fd)
{
	int flags = fcntl(fd, F_GETFD);

	return flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0 ? -1 : 0;
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

static void wm_draw_window(wm_state *state, const wm_client *client)
{
	uint32_t committed_width = 0;
	uint32_t committed_height = 0;
	int outer_x = client->x - (int)WM_WINDOW_BORDER;
	int outer_y = client->y - (int)WM_TITLE_HEIGHT - (int)WM_WINDOW_BORDER;

	/* Content coordinates remain client-visible; only the server owns chrome. */
	gui_fill_rect(state->composite, (gui_rect) { .x = outer_x, .y = outer_y,
		.w = (int)client->width + 2 * WM_WINDOW_BORDER,
		.h = (int)client->height + WM_TITLE_HEIGHT + 2 * WM_WINDOW_BORDER },
		WM_WINDOW_BORDER_COLOR);
	gui_fill_rect(state->composite, (gui_rect) { .x = client->x,
		.y = client->y - WM_TITLE_HEIGHT, .w = (int)client->width,
		.h = WM_TITLE_HEIGHT }, client->focused ? WM_WINDOW_TITLE_ACTIVE_COLOR :
		WM_WINDOW_TITLE_INACTIVE_COLOR);
	gui_fill_rect(state->composite, (gui_rect) { .x = client->x +
		(int)client->width - (int)WM_CLOSE_BUTTON_SIZE,
		.y = client->y - (int)WM_TITLE_HEIGHT,
		.w = WM_CLOSE_BUTTON_SIZE, .h = WM_CLOSE_BUTTON_SIZE }, WM_WINDOW_BORDER_COLOR);
	gui_fill_rect(state->composite, (gui_rect) { .x = client->x, .y = client->y,
		.w = (int)client->width, .h = (int)client->height }, WM_WINDOW_CONTENT_COLOR);
	if (client->committed == NULL)
		return;
	committed_width = client->committed->width < (int)client->width ?
		(uint32_t)client->committed->width : client->width;
	committed_height = client->committed->height < (int)client->height ?
		(uint32_t)client->committed->height : client->height;
	gui_blit(state->composite, client->x, client->y, client->committed,
		&(gui_rect) { .x = 0, .y = 0, .w = (int)committed_width,
			.h = (int)committed_height });
}

static gui_flush_result wm_compose(wm_server *server)
{
	wm_state *state = &server->state;
	gui_surface *desktop = server->desktop_snapshot;

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
	/* Select the next layer by serial so a freed client slot cannot reorder windows. */
	for (uint64_t painted = 0;;) {
		wm_client *next = NULL;

		for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
			wm_client *client = &server->clients[index];

			if (client->fd >= 0 && client->window_id != 0 &&
			    client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
			    client->stack_order > painted &&
			    (next == NULL || client->stack_order < next->stack_order))
				next = client;
		}
		if (next == NULL)
			break;
		wm_draw_window(state, next);
		painted = next->stack_order;
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
	if (mark_close_on_exec(fd) < 0) {
		close(fd);
		return NULL;
	}
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

static void wm_place_window(wm_server *server, wm_client *client)
{
	uint64_t layer = server->next_placement++;
	uint64_t wanted_x = 64 + layer * 32;
	uint64_t wanted_y = 64 + layer * 16;
	uint32_t maximum_x = client->width < server->state.width ?
		server->state.width - client->width : 0;
	uint32_t maximum_y = client->height < server->state.height ?
		server->state.height - client->height : 0;

	/* Cascade predictable initial rectangles while keeping every surface on-screen. */
	client->x = wanted_x > maximum_x ? maximum_x : (int32_t)wanted_x;
	client->y = wanted_y > maximum_y ? maximum_y : (int32_t)wanted_y;
	client->stack_order = ++server->next_stack_order;
}

static uint32_t wm_minimum_width(const wm_server *server)
{
	uint32_t maximum = server->state.width > 2 * WM_WINDOW_BORDER ?
		server->state.width - 2 * WM_WINDOW_BORDER : 1;

	return maximum < WM_MIN_CONTENT_WIDTH ? maximum : WM_MIN_CONTENT_WIDTH;
}

static uint32_t wm_minimum_height(const wm_server *server)
{
	uint32_t chrome_height = WM_TITLE_HEIGHT + 2 * WM_WINDOW_BORDER;
	uint32_t maximum = server->state.height > chrome_height ?
		server->state.height - chrome_height : 1;

	return maximum < WM_MIN_CONTENT_HEIGHT ? maximum : WM_MIN_CONTENT_HEIGHT;
}

static int32_t wm_clamp_position(int64_t value, uint32_t size, uint32_t screen_size,
				 uint32_t leading_chrome)
{
	int64_t minimum = leading_chrome;
	int64_t maximum = screen_size > size + WM_WINDOW_BORDER ?
		(int64_t)screen_size - size - WM_WINDOW_BORDER : minimum;

	if (value < minimum)
		return (int32_t)minimum;
	if (value > maximum)
		return (int32_t)maximum;
	return (int32_t)value;
}

static void wm_clamp_window_position(wm_server *server, wm_client *client)
{
	client->x = wm_clamp_position(client->x, client->width, server->state.width,
		WM_WINDOW_BORDER);
	client->y = wm_clamp_position(client->y, client->height, server->state.height,
		WM_TITLE_HEIGHT + WM_WINDOW_BORDER);
}

static uint32_t wm_clamp_dimension(int64_t value, uint32_t minimum,
				   uint32_t maximum)
{
	if (maximum < minimum)
		maximum = minimum;
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return (uint32_t)value;
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

static void client_handle_launch_request(wm_client *client, const uint8_t *bytes,
					 size_t payload_bytes)
{
	uint8_t result[sizeof(gui_protocol_wire_launch_result)];
	uint32_t request_id;
	uint16_t launch_type;
	uint16_t path_bytes;

	if (payload_bytes < sizeof(gui_protocol_wire_launch_request)) {
		client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
		return;
	}
	request_id = get_u32(bytes);
	launch_type = get_u16(bytes + 4);
	path_bytes = get_u16(bytes + 6);
	if (request_id == 0 || path_bytes > GUI_PROTOCOL_MAX_PATH_BYTES ||
	    payload_bytes != sizeof(gui_protocol_wire_launch_request) + path_bytes) {
		client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
		return;
	}
	switch (launch_type) {
	case GUI_PROTOCOL_LAUNCH_TERMINAL:
		if (path_bytes != 0) {
			client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
			return;
		}
		if (wm_start_terminal() < 0) {
			put_u32(result, request_id);
			put_u32(result + 4, (uint32_t)-errno);
			if (client_enqueue(client, GUI_PROTOCOL_LAUNCH_RESULT, result,
				   sizeof(result)) < 0)
				client->hello = -1;
			return;
		}
		put_u32(result, request_id);
		put_u32(result + 4, 0);
		if (client_enqueue(client, GUI_PROTOCOL_LAUNCH_RESULT, result,
			   sizeof(result)) < 0)
			client->hello = -1;
		return;
	case GUI_PROTOCOL_LAUNCH_FILE_MANAGER:
	case GUI_PROTOCOL_LAUNCH_IMAGE_VIEWER:
	case GUI_PROTOCOL_LAUNCH_OPEN_TEXT:
	case GUI_PROTOCOL_LAUNCH_EXECUTABLE:
		if (path_bytes == 0 || bytes[sizeof(gui_protocol_wire_launch_request)] != '/' ||
		    memchr(bytes + sizeof(gui_protocol_wire_launch_request), '\0', path_bytes) != NULL) {
			client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
			return;
		}
		break;
	default:
		client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
		return;
	}
	/* Only later milestones map these approved requests to server-owned children. */
	put_u32(result, request_id);
	put_u32(result + 4, (uint32_t)-ENOSYS);
	if (client_enqueue(client, GUI_PROTOCOL_LAUNCH_RESULT, result, sizeof(result)) < 0)
		client->hello = -1;
}

static void wm_notify_shell_window(wm_server *server, gui_protocol_type type,
				   const wm_client *window)
{
	uint8_t message[sizeof(gui_protocol_wire_window_list)] = { 0 };

	/* The private shell tracks ordinary taskbar targets, never itself. */
	if (window->role != GUI_PROTOCOL_ENDPOINT_ORDINARY || window->window_id == 0)
		return;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *shell = &server->clients[index];

		/* Bootstrap sends only the create reply and its following snapshot. */
		if (shell->fd < 0 || shell->role != GUI_PROTOCOL_ENDPOINT_SHELL ||
		    shell->hello <= 0 || shell->window_id == 0)
			continue;
		put_u32(message, window->window_id);
		put_u32(message + 4, window->x);
		put_u32(message + 8, window->y);
		put_u32(message + 12, window->width);
		put_u32(message + 16, window->height);
		put_u32(message + 20, window->focused);
		put_u32(message + 24, window->stack_order);
		if (client_enqueue(shell, type, message, sizeof(message)) < 0)
			shell->hello = -1;
	}
}

static void wm_send_shell_snapshot(wm_server *server, wm_client *shell)
{
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *window = &server->clients[index];
		uint8_t message[sizeof(gui_protocol_wire_window_list)] = { 0 };

		if (window->fd < 0 || window->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
		    window->window_id == 0)
			continue;
		put_u32(message, window->window_id);
		put_u32(message + 4, window->x);
		put_u32(message + 8, window->y);
		put_u32(message + 12, window->width);
		put_u32(message + 16, window->height);
		put_u32(message + 20, window->focused);
		put_u32(message + 24, window->stack_order);
		if (client_enqueue(shell, GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT, message,
			   sizeof(message)) < 0) {
			shell->hello = -1;
			return;
		}
	}
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
	if (client->role == GUI_PROTOCOL_ENDPOINT_SHELL) {
		/* Keep the old desktop on screen until this complete replacement is visible. */
		if (client->server->desktop_snapshot != NULL) {
			client->server->state.surface_bytes -=
				client->server->desktop_snapshot_bytes;
			gui_surface_destroy(client->server->desktop_snapshot);
			client->server->desktop_snapshot = NULL;
			client->server->desktop_snapshot_bytes = 0;
		}
		client->server->shell_ready = 1;
	}
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

static int client_enqueue_configure(wm_client *client)
{
	uint8_t configure[sizeof(gui_protocol_wire_window_configure)];

	put_u32(configure, client->window_id);
	put_u64(configure + 4, client->configure_serial);
	put_u32(configure + 12, client->width);
	put_u32(configure + 16, client->height);
	return client_enqueue(client, GUI_PROTOCOL_WINDOW_CONFIGURE, configure,
		sizeof(configure));
}

static void client_enqueue_close_request(wm_client *client)
{
	uint8_t close_request[sizeof(gui_protocol_wire_window_id)];

	if (client == NULL || client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
	    client->window_id == 0)
		return;
	put_u32(close_request, client->window_id);
	if (client_enqueue(client, GUI_PROTOCOL_WINDOW_CLOSE_REQUEST, close_request,
			   sizeof(close_request)) < 0)
		client->hello = -1;
}

static int wm_resize_window(wm_server *server, wm_client *client, int32_t x,
			    int32_t y, uint32_t width, uint32_t height)
{
	int32_t previous_x = client->x;
	int32_t previous_y = client->y;
	uint32_t previous_width = client->width;
	uint32_t previous_height = client->height;
	uint32_t minimum_width = wm_minimum_width(server);
	uint32_t minimum_height = wm_minimum_height(server);
	uint32_t maximum_width = server->state.width > 2 * WM_WINDOW_BORDER ?
		server->state.width - 2 * WM_WINDOW_BORDER : minimum_width;
	uint32_t maximum_height = server->state.height >
		WM_TITLE_HEIGHT + 2 * WM_WINDOW_BORDER ? server->state.height -
		(WM_TITLE_HEIGHT + 2 * WM_WINDOW_BORDER) : minimum_height;
	uint32_t position_width_limit;
	uint32_t position_height_limit;

	width = wm_clamp_dimension(width, minimum_width, maximum_width);
	height = wm_clamp_dimension(height, minimum_height, maximum_height);
	client->x = x;
	client->y = y;
	client->width = width;
	client->height = height;
	wm_clamp_window_position(server, client);
	position_width_limit = server->state.width > (uint32_t)client->x +
		WM_WINDOW_BORDER ? server->state.width - client->x - WM_WINDOW_BORDER :
		minimum_width;
	position_height_limit = server->state.height > (uint32_t)client->y +
		WM_WINDOW_BORDER ? server->state.height - client->y - WM_WINDOW_BORDER :
		minimum_height;
	client->width = wm_clamp_dimension(client->width, minimum_width,
		position_width_limit);
	client->height = wm_clamp_dimension(client->height, minimum_height,
		position_height_limit);
	if (client->x == previous_x && client->y == previous_y &&
	    client->width == previous_width && client->height == previous_height)
		return 0;
	if (client->configure_serial == UINT64_MAX)
		return -EOVERFLOW;
	client_discard_staging(client);
	client->configure_serial++;
	if (client_enqueue_configure(client) < 0)
		return -EIO;
	wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
	return 1;
}

static int wm_move_window(wm_server *server, wm_client *client, int32_t x, int32_t y)
{
	int32_t previous_x = client->x;
	int32_t previous_y = client->y;

	client->x = x;
	client->y = y;
	wm_clamp_window_position(server, client);
	if (client->x != previous_x || client->y != previous_y)
		wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
	return client->x != previous_x || client->y != previous_y;
}

typedef enum {
	WM_HIT_NONE,
	WM_HIT_CONTENT,
	WM_HIT_TITLE,
	WM_HIT_CLOSE,
	WM_HIT_RESIZE_LEFT,
	WM_HIT_RESIZE_RIGHT,
	WM_HIT_RESIZE_TOP,
	WM_HIT_RESIZE_BOTTOM,
	WM_HIT_RESIZE_TOP_LEFT,
	WM_HIT_RESIZE_TOP_RIGHT,
	WM_HIT_RESIZE_BOTTOM_LEFT,
	WM_HIT_RESIZE_BOTTOM_RIGHT,
} wm_hit_region;

static wm_hit_region wm_hit_region_for_client(const wm_client *client, uint32_t x,
					       uint32_t y)
{
	int64_t left = (int64_t)client->x - WM_WINDOW_BORDER;
	int64_t top = (int64_t)client->y - WM_TITLE_HEIGHT - WM_WINDOW_BORDER;
	int64_t right = (int64_t)client->x + client->width;
	int64_t bottom = (int64_t)client->y + client->height;

	if ((int64_t)x < left || (int64_t)x > right || (int64_t)y < top ||
	    (int64_t)y > bottom)
		return WM_HIT_NONE;
	if ((int64_t)x == left)
		return (int64_t)y == top ? WM_HIT_RESIZE_TOP_LEFT :
			(int64_t)y == bottom ? WM_HIT_RESIZE_BOTTOM_LEFT : WM_HIT_RESIZE_LEFT;
	if ((int64_t)x == right)
		return (int64_t)y == top ? WM_HIT_RESIZE_TOP_RIGHT :
			(int64_t)y == bottom ? WM_HIT_RESIZE_BOTTOM_RIGHT : WM_HIT_RESIZE_RIGHT;
	if ((int64_t)y == top)
		return WM_HIT_RESIZE_TOP;
	if ((int64_t)y == bottom)
		return WM_HIT_RESIZE_BOTTOM;
	if ((int64_t)y < client->y)
		return x >= (uint32_t)client->x + client->width - WM_CLOSE_BUTTON_SIZE ?
			WM_HIT_CLOSE : WM_HIT_TITLE;
	return WM_HIT_CONTENT;
}

static wm_client *wm_hit_test(wm_server *server, uint32_t x, uint32_t y,
				      wm_hit_region *region)
{
	wm_client *hit = NULL;
	wm_hit_region hit_region = WM_HIT_NONE;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];
		wm_hit_region candidate_region;

		if (client->fd < 0 || client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
		    client->window_id == 0 ||
		    (candidate_region = wm_hit_region_for_client(client, x, y)) == WM_HIT_NONE)
			continue;
		if (hit == NULL || client->stack_order > hit->stack_order) {
			hit = client;
			hit_region = candidate_region;
		}
	}
	if (region != NULL)
		*region = hit_region;
	return hit;
}

static void wm_cancel_capture(wm_server *server)
{
	server->capture_client = NULL;
	server->capture_operation = WM_CAPTURE_NONE;
}

static void wm_begin_capture(wm_server *server, wm_client *client,
			     wm_capture_operation operation)
{
	server->capture_client = client;
	server->capture_operation = operation;
	server->capture_x = server->state.cursor_x;
	server->capture_y = server->state.cursor_y;
	server->capture_window_x = client->x;
	server->capture_window_y = client->y;
	server->capture_width = client->width;
	server->capture_height = client->height;
}

static wm_client *wm_shell_client(wm_server *server)
{
	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		if (client->fd >= 0 && client->hello > 0 &&
		    client->role == GUI_PROTOCOL_ENDPOINT_SHELL && client->window_id != 0)
			return client;
	}
	return NULL;
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
		if (previous->role == GUI_PROTOCOL_ENDPOINT_ORDINARY) {
			previous->focused = 0;
			if (client_enqueue_focus(previous, 0) < 0)
				previous->hello = -1;
			wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, previous);
		}
		if (client_enqueue_input_reset(previous) < 0)
			previous->hello = -1;
	}
	server->input_owner = client;
	if (client != NULL) {
		if (client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY) {
			client->stack_order = ++server->next_stack_order;
			client->focused = 1;
			if (client_enqueue_focus(client, 1) < 0)
				client->hello = -1;
			wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
		}
		if (client_enqueue_input_reset(client) < 0)
			client->hello = -1;
	}
}

static wm_client *wm_next_focus(wm_server *server)
{
	wm_client *next = NULL;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *client = &server->clients[index];

		if (client->fd >= 0 && client->hello > 0 &&
		    client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
		    client->window_id != 0 &&
		    (next == NULL || client->stack_order > next->stack_order))
			next = client;
	}
	return next;
}

static void wm_disconnect_client(wm_server *server, wm_client *client)
{
	if (server->capture_client == client)
		wm_cancel_capture(server);
	if (client->role == GUI_PROTOCOL_ENDPOINT_SHELL) {
		if (server->shell_replacement_started || !server->shell_ready ||
		    client->committed == NULL ||
		    server->desktop_snapshot != NULL) {
			client_destroy(client);
			stop_requested = 1;
			return;
		}
		/* The last complete shell frame remains input-inert during replacement. */
		server->desktop_snapshot = client->committed;
		server->desktop_snapshot_bytes = client->committed_bytes;
		client->committed = NULL;
		client->committed_bytes = 0;
		if (server->input_owner == client)
			wm_set_input_owner(server, wm_next_focus(server));
		client_destroy(client);
		server->shell_ready = 0;
		/* The session permits one recovery attempt, never an unbounded respawn loop. */
		server->shell_replacement_started = 1;
		if (wm_start_shell(server) < 0 || wm_compose(server) == GUI_FLUSH_ERROR)
			stop_requested = 1;
		return;
	}
	if (server->input_owner == client) {
		/* Focus loss must precede removal so the shell never sees a stale change. */
		wm_set_input_owner(server, NULL);
		wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_REMOVE, client);
		client_destroy(client);
		wm_set_input_owner(server, wm_next_focus(server));
		return;
	}
	wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_REMOVE, client);
	client_destroy(client);
}

static void wm_reset_key_state(wm_state *state)
{
	state->modifiers = 0;
	memset(state->key_down, 0, sizeof(state->key_down));
}

static void wm_update_modifiers(wm_state *state, uint16_t scancode, int pressed)
{
	uint32_t modifier = 0;

	switch (scancode) {
	case 0x2a:
	case 0x36:
		modifier = GUI_MOD_SHIFT;
		break;
	case 0x1d:
	case 0x11d:
		modifier = GUI_MOD_CTRL;
		break;
	case 0x38:
	case 0x138:
		modifier = GUI_MOD_ALT;
		break;
	case 0x15b:
	case 0x15c:
		modifier = GUI_MOD_SUPER;
		break;
	case 0x3a:
		if (pressed && !state->key_down[scancode])
			state->modifiers ^= GUI_MOD_CAPS_LOCK;
		break;
	default:
		break;
	}
	if (modifier != 0) {
		if (pressed)
			state->modifiers |= modifier;
		else
			state->modifiers &= ~modifier;
	}
	if (scancode < sizeof(state->key_down))
		state->key_down[scancode] = pressed;
}

static uint32_t wm_keysym(uint16_t scancode, uint32_t modifiers)
{
	static const struct {
		uint16_t scancode;
		char plain;
		char shifted;
	} punctuation[] = {
		{ 0x02, '1', '!' }, { 0x03, '2', '@' }, { 0x04, '3', '#' },
		{ 0x05, '4', '$' }, { 0x06, '5', '%' }, { 0x07, '6', '^' },
		{ 0x08, '7', '&' }, { 0x09, '8', '*' }, { 0x0a, '9', '(' },
		{ 0x0b, '0', ')' }, { 0x0c, '-', '_' }, { 0x0d, '=', '+' },
		{ 0x1a, '[', '{' }, { 0x1b, ']', '}' }, { 0x27, ';', ':' },
		{ 0x28, '\'', '"' }, { 0x29, '`', '~' }, { 0x2b, '\\', '|' },
		{ 0x33, ',', '<' }, { 0x34, '.', '>' }, { 0x35, '/', '?' },
	};
	static const struct {
		uint16_t scancode;
		char letter;
	} letters[] = {
		{ 0x10, 'q' }, { 0x11, 'w' }, { 0x12, 'e' }, { 0x13, 'r' },
		{ 0x14, 't' }, { 0x15, 'y' }, { 0x16, 'u' }, { 0x17, 'i' },
		{ 0x18, 'o' }, { 0x19, 'p' }, { 0x1e, 'a' }, { 0x1f, 's' },
		{ 0x20, 'd' }, { 0x21, 'f' }, { 0x22, 'g' }, { 0x23, 'h' },
		{ 0x24, 'j' }, { 0x25, 'k' }, { 0x26, 'l' }, { 0x2c, 'z' },
		{ 0x2d, 'x' }, { 0x2e, 'c' }, { 0x2f, 'v' }, { 0x30, 'b' },
		{ 0x31, 'n' }, { 0x32, 'm' },
	};
	int shifted = !!(modifiers & GUI_MOD_SHIFT);

	for (size_t index = 0; index < sizeof(letters) / sizeof(letters[0]); index++) {
		if (letters[index].scancode == scancode) {
			if (shifted ^ !!(modifiers & GUI_MOD_CAPS_LOCK))
				return (uint32_t)(letters[index].letter - 'a' + 'A');
			return letters[index].letter;
		}
	}
	for (size_t index = 0; index < sizeof(punctuation) / sizeof(punctuation[0]); index++) {
		if (punctuation[index].scancode == scancode)
			return (unsigned char)(shifted ? punctuation[index].shifted :
				punctuation[index].plain);
	}
	switch (scancode) {
	case 0x01: return GUI_KEYSYM_ESCAPE;
	case 0x0e: return GUI_KEYSYM_BACKSPACE;
	case 0x0f: return '\t';
	case 0x1c: return '\n';
	case 0x39: return ' ';
	case 0x147: return GUI_KEYSYM_HOME;
	case 0x149: return GUI_KEYSYM_PAGE_UP;
	case 0x14b: return GUI_KEYSYM_LEFT;
	case 0x14d: return GUI_KEYSYM_RIGHT;
	case 0x14f: return GUI_KEYSYM_END;
	case 0x150: return GUI_KEYSYM_DOWN;
	case 0x151: return GUI_KEYSYM_PAGE_DOWN;
	case 0x152: return GUI_KEYSYM_INSERT;
	case 0x153: return GUI_KEYSYM_DELETE;
	case 0x2a:
	case 0x36: return GUI_KEYSYM_SHIFT;
	case 0x1d:
	case 0x11d: return GUI_KEYSYM_CTRL;
	case 0x38:
	case 0x138: return GUI_KEYSYM_ALT;
	case 0x15b:
	case 0x15c: return GUI_KEYSYM_SUPER;
	default: return 0;
	}
}

static wm_client *wm_alt_tab_target(wm_server *server)
{
	wm_client *current = server->input_owner;
	wm_client *next = NULL;
	wm_client *wrap = NULL;

	for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
		wm_client *candidate = &server->clients[index];

		if (candidate->fd < 0 || candidate->hello <= 0 ||
		    candidate->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
		    candidate->window_id == 0)
			continue;
		if (wrap == NULL || candidate->stack_order > wrap->stack_order)
			wrap = candidate;
		if (current != NULL && current->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
		    candidate->stack_order < current->stack_order &&
		    (next == NULL || candidate->stack_order > next->stack_order))
			next = candidate;
	}
	return next != NULL ? next : wrap;
}

static wm_capture_operation wm_capture_operation_for_region(wm_hit_region region)
{
	switch (region) {
	case WM_HIT_TITLE: return WM_CAPTURE_MOVE;
	case WM_HIT_RESIZE_LEFT: return WM_CAPTURE_RESIZE_LEFT;
	case WM_HIT_RESIZE_RIGHT: return WM_CAPTURE_RESIZE_RIGHT;
	case WM_HIT_RESIZE_TOP: return WM_CAPTURE_RESIZE_TOP;
	case WM_HIT_RESIZE_BOTTOM: return WM_CAPTURE_RESIZE_BOTTOM;
	case WM_HIT_RESIZE_TOP_LEFT: return WM_CAPTURE_RESIZE_TOP_LEFT;
	case WM_HIT_RESIZE_TOP_RIGHT: return WM_CAPTURE_RESIZE_TOP_RIGHT;
	case WM_HIT_RESIZE_BOTTOM_LEFT: return WM_CAPTURE_RESIZE_BOTTOM_LEFT;
	case WM_HIT_RESIZE_BOTTOM_RIGHT: return WM_CAPTURE_RESIZE_BOTTOM_RIGHT;
	default: return WM_CAPTURE_NONE;
	}
}

static int wm_update_capture(wm_server *server)
{
	wm_client *client = server->capture_client;
	int64_t dx = (int64_t)server->state.cursor_x - server->capture_x;
	int64_t dy = (int64_t)server->state.cursor_y - server->capture_y;
	int32_t x = server->capture_window_x;
	int32_t y = server->capture_window_y;
	uint32_t width = server->capture_width;
	uint32_t height = server->capture_height;
	int resize = 0;

	if (client == NULL || client->fd < 0 || client->window_id == 0)
		return 0;
	if (server->capture_operation == WM_CAPTURE_MOVE)
		return wm_move_window(server, client, (int32_t)(x + dx), (int32_t)(y + dy));
	if (server->capture_operation == WM_CAPTURE_RESIZE_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_BOTTOM_LEFT) {
		int64_t right = (int64_t)x + width;
		int64_t maximum_x = right - wm_minimum_width(server);
		int64_t proposed_x = (int64_t)x + dx;

		if (proposed_x < WM_WINDOW_BORDER)
			proposed_x = WM_WINDOW_BORDER;
		if (proposed_x > maximum_x)
			proposed_x = maximum_x;
		x = (int32_t)proposed_x;
		width = (uint32_t)(right - proposed_x);
		resize = 1;
	}
	if (server->capture_operation == WM_CAPTURE_RESIZE_RIGHT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_RIGHT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_BOTTOM_RIGHT) {
		width = wm_clamp_dimension((int64_t)width + dx, wm_minimum_width(server),
			server->state.width > (uint32_t)x + WM_WINDOW_BORDER ?
			server->state.width - x - WM_WINDOW_BORDER : wm_minimum_width(server));
		resize = 1;
	}
	if (server->capture_operation == WM_CAPTURE_RESIZE_TOP ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_RIGHT) {
		int64_t bottom = (int64_t)y + height;
		int64_t maximum_y = bottom - wm_minimum_height(server);
		int64_t proposed_y = (int64_t)y + dy;

		if (proposed_y < WM_TITLE_HEIGHT + WM_WINDOW_BORDER)
			proposed_y = WM_TITLE_HEIGHT + WM_WINDOW_BORDER;
		if (proposed_y > maximum_y)
			proposed_y = maximum_y;
		y = (int32_t)proposed_y;
		height = (uint32_t)(bottom - proposed_y);
		resize = 1;
	}
	if (server->capture_operation == WM_CAPTURE_RESIZE_BOTTOM ||
	    server->capture_operation == WM_CAPTURE_RESIZE_BOTTOM_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_BOTTOM_RIGHT) {
		height = wm_clamp_dimension((int64_t)height + dy, wm_minimum_height(server),
			server->state.height > (uint32_t)y + WM_WINDOW_BORDER ?
			server->state.height - y - WM_WINDOW_BORDER : wm_minimum_height(server));
		resize = 1;
	}
	if (!resize || (x == client->x && y == client->y && width == client->width &&
			height == client->height))
		return 0;
	return wm_resize_window(server, client, x, y, width, height) > 0;
}

static void wm_emit_input(void *context, const gui_input_event *event)
{
	wm_server *server = context;
	wm_state *state = &server->state;
	wm_client *client;
	uint8_t message[sizeof(gui_protocol_wire_input_event)] = { 0 };
	uint32_t keysym = 0;
	int32_t x;
	int32_t y;
	int compose = 0;

	if (event->type == GUI_INPUT_EVENT_KEY_RESET) {
		wm_reset_key_state(state);
		wm_cancel_capture(server);
		wm_reset_input_owner(server);
		return;
	}
	if (event->type == GUI_INPUT_EVENT_MOUSE_RESET) {
		state->mouse_buttons = 0;
		wm_cancel_capture(server);
		wm_reset_input_owner(server);
		return;
	}
	if (!server->state.graphics)
		return;
	if (event->type == GUI_INPUT_EVENT_KEY) {
		wm_update_modifiers(state, event->key.scancode, event->key.pressed);
		if (!event->key.pressed)
			return;
		keysym = wm_keysym(event->key.scancode, state->modifiers);
		if (keysym == '\t' && (state->modifiers & GUI_MOD_ALT)) {
			wm_client *target = wm_alt_tab_target(server);

			if (target != NULL) {
				wm_set_input_owner(server, target);
				compose = 1;
			}
			if (compose && wm_compose(server) == GUI_FLUSH_ERROR)
				stop_requested = 1;
			return;
		}
		if (keysym == 'w' && (state->modifiers & GUI_MOD_SUPER)) {
			client_enqueue_close_request(server->input_owner);
			return;
		}
		if (keysym == 0)
			return;
	}
	if (event->type == GUI_INPUT_EVENT_MOUSE) {
		uint8_t previous_buttons = state->mouse_buttons;
		int64_t cursor_x = (int64_t)state->cursor_x + event->mouse.dx;
		int64_t cursor_y = (int64_t)state->cursor_y - event->mouse.dy;

		if (cursor_x < 0)
			cursor_x = 0;
		else if ((uint64_t)cursor_x >= state->width)
			cursor_x = state->width - 1;
		if (cursor_y < 0)
			cursor_y = 0;
		else if ((uint64_t)cursor_y >= state->height)
			cursor_y = state->height - 1;
		if (state->cursor_x != (uint32_t)cursor_x || state->cursor_y != (uint32_t)cursor_y) {
			state->cursor_x = cursor_x;
			state->cursor_y = cursor_y;
			compose = 1;
		}
		state->mouse_buttons = event->mouse.buttons;
		if (server->capture_client != NULL) {
			if (state->mouse_buttons & 1)
				compose |= wm_update_capture(server);
			else
				wm_cancel_capture(server);
			if (compose && wm_compose(server) == GUI_FLUSH_ERROR)
				stop_requested = 1;
			return;
		}
		if (!(previous_buttons & 1) && (state->mouse_buttons & 1)) {
			wm_hit_region region;
			wm_client *hit = wm_hit_test(server, state->cursor_x, state->cursor_y, &region);
			wm_capture_operation operation = wm_capture_operation_for_region(region);

			if (hit == NULL)
				hit = wm_shell_client(server);
			if (hit != NULL) {
				wm_set_input_owner(server, hit);
				compose = 1;
			}
			if (hit != NULL && hit->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
			    operation != WM_CAPTURE_NONE) {
				wm_begin_capture(server, hit, operation);
				if (compose && wm_compose(server) == GUI_FLUSH_ERROR)
					stop_requested = 1;
				return;
			}
			if (hit != NULL && hit->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
			    region == WM_HIT_CLOSE) {
				client_enqueue_close_request(hit);
				if (compose && wm_compose(server) == GUI_FLUSH_ERROR)
					stop_requested = 1;
				return;
			}
		}
		if (compose && wm_compose(server) == GUI_FLUSH_ERROR)
				stop_requested = 1;
	}
	client = server->input_owner;
	if (client == NULL)
		return;
	put_u32(message, client->window_id);
	put_u32(message + 4, event->type);
	if (event->type == GUI_INPUT_EVENT_KEY)
		put_u32(message + 8, keysym);
	put_u32(message + 12, state->modifiers);
	x = state->cursor_x;
	y = state->cursor_y;
	if (client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY) {
		x -= client->x;
		y -= client->y;
	}
	put_u32(message + 16, (uint32_t)x);
	put_u32(message + 20, (uint32_t)y);
	if (event->type == GUI_INPUT_EVENT_MOUSE)
		message[24] = event->mouse.buttons;
	put_u32(message + 28, event->ticks);
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
	size_t window_bytes;

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
	if (header->type == GUI_PROTOCOL_LAUNCH_REQUEST) {
		client_handle_launch_request(client, bytes, payload_bytes);
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CREATE) {
		if (client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
			client_reject(client, GUI_PROTOCOL_WINDOW_CREATE, -EPERM);
		else if (client->window_id != 0)
			client_reject(client, GUI_PROTOCOL_WINDOW_CREATE, -EBUSY);
		else if (payload_bytes < sizeof(gui_protocol_wire_window_create) ||
			 payload_bytes != sizeof(gui_protocol_wire_window_create) +
			 get_u16(bytes + 12) || get_u32(bytes + 4) == 0 ||
			 get_u32(bytes + 8) == 0 ||
			 surface_bytes(get_u32(bytes + 4), get_u32(bytes + 8),
				       &window_bytes) < 0 ||
			 get_u32(bytes + 4) > client->server->state.width ||
			 get_u32(bytes + 8) > client->server->state.height) {
			client_reject(client, GUI_PROTOCOL_WINDOW_CREATE, -EINVAL);
		} else {
		client->window_id = wm_allocate_window_id(client->server);
		if (client->window_id == 0) {
			client_reject(client, GUI_PROTOCOL_WINDOW_CREATE, -EOVERFLOW);
			return;
		}
		client->width = get_u32(bytes + 4);
		client->height = get_u32(bytes + 8);
		wm_place_window(client->server, client);
		client->configure_serial = 1;
		put_u32(reply, get_u32(bytes));
		put_u32(reply + 4, client->window_id);
		put_u64(reply + 8, 1);
		put_u32(reply + 16, get_u32(bytes + 4));
		put_u32(reply + 20, get_u32(bytes + 8));
		if (client_enqueue(client, GUI_PROTOCOL_WINDOW_CREATE_REPLY, reply,
				   sizeof(reply)) < 0)
			client->hello = -1;
		else {
			wm_notify_shell_window(client->server, GUI_PROTOCOL_WINDOW_LIST_ADD, client);
			wm_set_input_owner(client->server, client);
		}
		}
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
			client->window_id = wm_allocate_window_id(client->server);
			if (client->window_id == 0) {
				client_reject(client, GUI_PROTOCOL_DESKTOP_CREATE, -EOVERFLOW);
				return;
			}
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
			else
				wm_send_shell_snapshot(client->server, client);
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_MOVE &&
	    payload_bytes == sizeof(gui_protocol_wire_window_move)) {
		if (client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
			client_reject(client, GUI_PROTOCOL_WINDOW_MOVE, -EPERM);
		else if (get_u32(bytes) != client->window_id || client->window_id == 0)
			client_reject(client, GUI_PROTOCOL_WINDOW_MOVE, -EINVAL);
		else {
			(void)wm_move_window(client->server, client, (int32_t)get_u32(bytes + 4),
				(int32_t)get_u32(bytes + 8));
			if (wm_compose(client->server) == GUI_FLUSH_ERROR)
				client->hello = -1;
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_RESIZE &&
	    payload_bytes == sizeof(gui_protocol_wire_window_resize)) {
		int result;

		if (client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
			client_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, -EPERM);
		else if (get_u32(bytes) != client->window_id || client->window_id == 0)
			client_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, -EINVAL);
		else {
			result = wm_resize_window(client->server, client, client->x, client->y,
				get_u32(bytes + 4), get_u32(bytes + 8));
			if (result < 0)
				client_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, result);
			else if (result > 0 && wm_compose(client->server) == GUI_FLUSH_ERROR)
				client->hello = -1;
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_DESTROY && payload_bytes == 4) {
		wm_server *server = client->server;

		if (get_u32(bytes) != client->window_id || client->window_id == 0) {
			client_reject(client, GUI_PROTOCOL_WINDOW_DESTROY, -EINVAL);
			return;
		}
		if (server->capture_client == client)
			wm_cancel_capture(server);
		if (server->input_owner == client)
			wm_set_input_owner(server, NULL);
		if (client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY)
			wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_REMOVE, client);
		client_discard_staging(client);
		client_discard_committed(client);
		client->window_id = 0;
		client->focused = 0;
		if (client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY)
			wm_set_input_owner(server, wm_next_focus(server));
		if (wm_compose(server) == GUI_FLUSH_ERROR)
			client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW && payload_bytes == 4) {
		if (client->role != GUI_PROTOCOL_ENDPOINT_SHELL) {
			client_reject(client, GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW, -EPERM);
		} else {
			wm_client *target = NULL;

			for (size_t index = 0; index < WM_CLIENT_LIMIT; index++) {
				wm_client *candidate = &client->server->clients[index];

				if (candidate->fd >= 0 &&
				    candidate->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
				    candidate->window_id == get_u32(bytes)) {
					target = candidate;
					break;
				}
			}
			if (target == NULL)
				client_reject(client, GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW, -ENOENT);
			else {
				wm_set_input_owner(client->server, target);
				if (wm_compose(client->server) == GUI_FLUSH_ERROR)
					client->hello = -1;
			}
		}
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
	if (mark_close_on_exec(listener) < 0) {
		int saved_errno = errno;

		close(listener);
		errno = saved_errno;
		return -1;
	}
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
	/* All server descriptors are close-on-exec; fd 3 is the shell's only capability. */
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

static int wm_start_terminal(void)
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		/* Server-owned descriptors are close-on-exec, so clients get no seat access. */
		execl(WM_TERMINAL_PATH, WM_TERMINAL_PATH, NULL);
		_exit(127);
	}
	return 0;
}

int main(void)
{
	struct sigaction action = { .sa_handler = request_stop };
	struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
	struct sigaction ignore_child = { .sa_handler = SIG_IGN };
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
	if (server.state.framebuffer < 0 || mark_close_on_exec(server.state.framebuffer) < 0)
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
	if (server.state.gui < 0 || mark_close_on_exec(server.state.gui) < 0)
		goto out;
	/* The seat owner reads both input streams so the cursor cannot be spoofed. */
	server.state.mouse = open("/dev/psaux", O_RDONLY | O_NONBLOCK);
	if (server.state.mouse < 0 || mark_close_on_exec(server.state.mouse) < 0)
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
	    sigaction(SIGPIPE, &ignore_pipe, NULL) < 0 ||
	    sigaction(SIGCHLD, &ignore_child, NULL) < 0)
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
			wm_cancel_capture(&server);
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
	if (server.desktop_snapshot != NULL) {
		server.state.surface_bytes -= server.desktop_snapshot_bytes;
		gui_surface_destroy(server.desktop_snapshot);
	}
	if (status != 0)
		perror("mario-wm");
	wm_cleanup(&server.state);
	return status;
}
