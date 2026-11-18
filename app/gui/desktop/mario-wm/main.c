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
#include <sys/wait.h>
#include <unistd.h>

#include <app/gui-session.h>
#include <app/gui-limits.h>
#include <mario/fb.h>
#include <mario/input.h>
#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>

#define WM_SHELL_SNAPSHOT_QUANTUM 8U
#define WM_CLIENT_INPUT_READ_QUANTUM 4096U
#define WM_INPUT_BATCH_RECORDS 16U
#define WM_INPUT_READ_QUANTUM 4U
#define WM_MOTION_FRAME_MS 16U
#define WM_STATUS_RENAME_ATTEMPTS 16U
#define WM_STATUS_RENAME_DELAY_US 10000U
#define WM_SHELL_PATH "/usr/bin/mario-desktop"
#define WM_SHELL_FD 3
#define WM_TERMINAL_PATH "/usr/bin/terminal"
#define WM_CURSOR_PATH "/usr/share/mario/cursor-arrow-16.rgba"
#define WM_CURSOR_WIDTH 16U
#define WM_CURSOR_HEIGHT 16U
#define WM_DIAGNOSTIC_COUNT 2U
#define WM_RESIZE_GRIP 4U
#define WM_TITLE_HEIGHT 24U
#define WM_TITLE_BUTTON_WIDTH 16U
#define WM_TITLE_DOUBLE_CLICK_MS 500U
#define WM_TITLE_CLICK_SLOP 4
#define WM_MIN_CONTENT_WIDTH 64U
#define WM_MIN_CONTENT_HEIGHT 48U
#define WM_WINDOW_BORDER_COLOR 0x000f172aU
#define WM_WINDOW_TITLE_ACTIVE_COLOR 0x002563ebU
#define WM_WINDOW_TITLE_INACTIVE_COLOR 0x00475569U
#define WM_WINDOW_CONTENT_COLOR 0x00f8fafcU

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t child_reap_requested;

static void request_stop(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void request_child_reap(int signal_number)
{
	(void)signal_number;
	child_reap_requested = 1;
}

typedef enum {
	WM_CURSOR_ARROW,
	WM_CURSOR_RESIZE_HORIZONTAL,
	WM_CURSOR_RESIZE_VERTICAL,
	WM_CURSOR_RESIZE_NW_SE,
	WM_CURSOR_RESIZE_NE_SW,
	WM_CURSOR_MOVE,
	WM_CURSOR_SHAPE_COUNT,
} wm_cursor_shape;

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
	wm_cursor_shape cursor_shape;
	uint32_t modifiers;
	uint8_t mouse_buttons;
	uint8_t key_down[512];
	uint8_t cursor_rgba[WM_CURSOR_WIDTH * WM_CURSOR_HEIGHT * 4];
	int cursor_loaded;
	gui_surface *composite;
	gui_region damage;
	gui_region_rect damage_rects[GUI_REGION_MAX_RECTS];
	int painted;
	uint32_t painted_cursor_x;
	uint32_t painted_cursor_y;
	wm_cursor_shape painted_cursor_shape;
	int painted_cursor_loaded;
	uint32_t painted_fullscreen;
	const void *painted_desktop;
	int motion_pending;
	int scanout_busy;
	uint64_t motion_deadline_ms;
} wm_state;

typedef struct wm_server wm_server;
typedef struct wm_client wm_client;
typedef struct wm_window wm_window;

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

typedef enum {
	WM_TITLE_BUTTON_NONE,
	WM_TITLE_BUTTON_MINIMIZE,
	WM_TITLE_BUTTON_MAXIMIZE,
	WM_TITLE_BUTTON_CLOSE,
} wm_title_button;

struct wm_window {
	wm_window *next;
	wm_window *owner_next;
	wm_client *client;
	int focused;
	int list_visible;
	gui_protocol_type list_pending;
	uint32_t window_id;
	uint32_t width;
	uint32_t height;
	int32_t x;
	int32_t y;
	uint64_t stack_order;
	uint64_t configure_serial;
	size_t reserved_bytes;
	int transition_pending;
	int fullscreen;
	int mode_pending;
	int previous_fullscreen;
	int minimized;
	int maximized;
	int state_pending;
	uint32_t pending_state;
	int32_t normal_x;
	int32_t normal_y;
	uint32_t normal_width;
	uint32_t normal_height;
	int32_t previous_x;
	int32_t previous_y;
	uint32_t previous_width;
	uint32_t previous_height;
	uint64_t staging_frame_serial;
	size_t staging_bytes;
	size_t staging_pixels;
	gui_surface *staging;
	int staging_patch;
	gui_rect staging_rectangle;
	uint64_t staging_base_frame_serial;
	uint64_t last_admitted_frame_serial;
	uint64_t committed_frame_serial;
	uint64_t committed_configure_serial;
	size_t committed_bytes;
	gui_surface *committed;
	wm_server *server;
	uint16_t title_bytes;
	uint8_t title[GUI_PROTOCOL_MAX_TITLE_BYTES];
	int painted;
	gui_rect painted_bounds;
	gui_rect painted_content;
	int painted_focus;
	wm_title_button painted_hover_button;
	wm_title_button painted_pressed_button;
	uint64_t painted_order;
	wm_title_button hover_button;
	wm_title_button pressed_button;
};

struct wm_client {
	wm_client *next;
	wm_server *server;
	wm_window *windows;
	int fd;
	int hello;
	uint16_t protocol_minor;
	uint32_t capabilities;
	gui_protocol_endpoint_role role;
	gui_protocol_input *input;
	gui_protocol_output *output;
	int drain_input;
	size_t record_bytes;
	size_t surface_bytes;
	uint32_t snapshot_cursor;
	uint32_t snapshot_end;
	uint32_t delta_cursor;
	int snapshot_pending;
};

typedef struct wm_launch {
	struct wm_launch *next;
	wm_client *client;
	pid_t pid;
	uint32_t origin_window_id;
} wm_launch;

struct wm_server {
	wm_state state;
	wm_client *clients;
	wm_window *windows;
	wm_window *removed_windows;
	size_t list_pending;
	wm_launch *launches;
	size_t record_bytes;
	size_t client_count;
	void *poll_storage;
	size_t poll_capacity;
	size_t poll_bytes;
	gui_surface *desktop_snapshot;
	size_t desktop_snapshot_bytes;
	uint32_t next_window_id;
	uint64_t next_placement;
	uint64_t next_stack_order;
	/* All focus and future grab transitions enter through this owner. */
	wm_window *input_owner;
	wm_window *fullscreen_window;
	int32_t work_x;
	int32_t work_y;
	uint32_t work_width;
	uint32_t work_height;
	int work_area_valid;
	uint32_t deferred_focus_id;
	/* Pointer capture keeps title and border drags out of client input streams. */
	wm_window *capture_client;
	wm_capture_operation capture_operation;
	uint32_t capture_x;
	uint32_t capture_y;
	int32_t capture_window_x;
	int32_t capture_window_y;
	uint32_t capture_width;
	uint32_t capture_height;
	uint32_t title_click_window_id;
	uint32_t title_click_x;
	uint32_t title_click_y;
	uint64_t title_click_ms;
	int title_click_released;
	/* The private shell is server-owned and must not outlive the server. */
	pid_t shell_pid;
	int shell_ready;
	int shell_replacement_started;
	int diagnostics;
};

static int wm_start_shell(wm_server *server);
static int wm_start_terminal(void);
static int wm_start_terminal_command(const uint8_t *path, size_t path_bytes);
static int wm_start_terminal_executable(wm_server *server, const uint8_t *path,
					size_t path_bytes, uint32_t origin_window_id);
static int client_enqueue_configure(wm_window *client);
static int window_enqueue(wm_window *window, gui_protocol_type type,
			  const void *payload, size_t payload_bytes);
static void window_reject(wm_window *window, gui_protocol_type type, int status);
static void wm_cancel_capture(wm_server *server);
static int wm_update_cursor(wm_server *server);
static void wm_reset_input_owner(wm_server *server);
static void wm_set_input_owner(wm_server *server, wm_window *client);
static wm_window *wm_next_focus(wm_server *server);
static void wm_complete_mode(wm_window *window);
static int wm_request_minimize(wm_server *server, wm_window *window);
static int wm_request_maximize(wm_server *server, wm_window *window, int maximized);
static int client_enqueue_window_state(wm_window *window, uint32_t state);

static void wm_damage_init(wm_state *state)
{
	if (state->damage.rects != NULL)
		return;
	gui_region_init(&state->damage, state->damage_rects, GUI_REGION_MAX_RECTS,
		(gui_rect) { 0, 0, state->width, state->height });
	gui_region_mark_full(&state->damage);
}

static void wm_damage_rect(wm_state *state, gui_rect rectangle)
{
	wm_damage_init(state);
	gui_region_add(&state->damage, rectangle);
}

static void *wm_record_create(wm_server *server, wm_client *client, size_t bytes)
{
	void *record;

	if (server->record_bytes > APP_GUI_WM_RECORD_BYTES ||
	    bytes > APP_GUI_WM_RECORD_BYTES - server->record_bytes ||
	    (client != NULL && (client->record_bytes > APP_GUI_PEER_RECORD_BYTES ||
	     bytes > APP_GUI_PEER_RECORD_BYTES - client->record_bytes))) {
		errno = ENOMEM;
		return NULL;
	}
	record = calloc(1, bytes);
	if (record != NULL) {
		server->record_bytes += bytes;
		if (client != NULL)
			client->record_bytes += bytes;
	}
	return record;
}

static void wm_record_destroy(wm_server *server, wm_client *client,
			      void *record, size_t bytes)
{
	server->record_bytes -= bytes;
	if (client != NULL)
		client->record_bytes -= bytes;
	free(record);
}

static int wm_poll_resize(wm_server *server, size_t capacity)
{
	size_t bytes;
	void *storage;

	if (capacity > (SIZE_MAX - 4 * sizeof(struct pollfd)) /
	    (sizeof(struct pollfd) + sizeof(wm_client *)))
		return -1;
	bytes = capacity == 0 ? 0 : (capacity + 4) * sizeof(struct pollfd) +
		capacity * sizeof(wm_client *);
	if (server->record_bytes - server->poll_bytes > APP_GUI_WM_RECORD_BYTES ||
	    bytes > APP_GUI_WM_RECORD_BYTES - (server->record_bytes - server->poll_bytes))
		return -1;
	storage = bytes == 0 ? NULL : realloc(server->poll_storage, bytes);
	if (bytes != 0 && storage == NULL)
		return -1;
	if (bytes == 0)
		free(server->poll_storage);
	server->record_bytes = server->record_bytes - server->poll_bytes + bytes;
	server->poll_storage = storage;
	server->poll_capacity = capacity;
	server->poll_bytes = bytes;
	return 0;
}

static int wm_poll_reserve(wm_server *server, size_t count)
{
	size_t capacity;

	if (count <= server->poll_capacity)
		return 0;
	capacity = server->poll_capacity + server->poll_capacity / 2 + 1;
	if (capacity < count)
		capacity = count;
	return wm_poll_resize(server, capacity) == 0 ? 0 : wm_poll_resize(server, count);
}

static wm_window *wm_find_window(wm_server *server, uint32_t window_id)
{
	for (wm_window *window = server->windows; window != NULL; window = window->next)
		if (window->window_id == window_id)
			return window;
	return NULL;
}

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
	for (unsigned int attempt = 0;; attempt++) {
		if (rename(temporary_path, path) == 0)
			return 0;
		int saved_errno = errno;

		if (saved_errno == EBUSY && attempt + 1 < WM_STATUS_RENAME_ATTEMPTS) {
			if (usleep(WM_STATUS_RENAME_DELAY_US) == 0 || errno == EINTR)
				continue;
			saved_errno = errno;
		}
		unlink(temporary_path);
		errno = saved_errno;
		return -1;
	}
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
			      (size_t)length) < 0) {
		fprintf(stderr, "mario-wm: frame publication failed: %d\n", errno);
		return -1;
	}
	state->frame_number = frame_number;
	/* Readiness is published only after the corresponding frame is visible. */
	if (write_status_file(WM_READY_PATH, WM_READY_PATH ".new", "ready\n", 6) < 0) {
		fprintf(stderr, "mario-wm: readiness publication failed: %d\n", errno);
		return -1;
	}
	return 0;
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

static void client_discard_staging(wm_window *client)
{
	if (client->staging != NULL) {
		client->server->state.surface_bytes -= client->staging_bytes;
		client->client->surface_bytes -= client->staging_bytes;
		gui_surface_destroy(client->staging);
		client->staging = NULL;
		client->staging_bytes = 0;
		client->staging_frame_serial = 0;
	}
	client->staging_pixels = 0;
	client->staging_patch = 0;
	client->staging_base_frame_serial = 0;
	client->staging_rectangle = (gui_rect) { 0 };
}

static void client_discard_committed(wm_window *client)
{
	if (client->committed != NULL) {
		client->server->state.surface_bytes -= client->committed_bytes;
		client->client->surface_bytes -= client->committed_bytes;
		gui_surface_destroy(client->committed);
		client->committed = NULL;
		client->committed_bytes = 0;
	}
}

static void client_discard_reservation(wm_window *client)
{
	if (client->reserved_bytes != 0) {
		client->server->state.surface_bytes -= client->reserved_bytes;
		client->client->surface_bytes -= client->reserved_bytes;
		client->reserved_bytes = 0;
	}
}

static void window_destroy(wm_window *window)
{
	wm_server *server = window->server;
	wm_client *client = window->client;
	wm_window **link = &server->windows;

	if (server->capture_client == window ||
	    server->title_click_window_id == window->window_id)
		wm_cancel_capture(server);
	if (window->painted)
		wm_damage_rect(&server->state, window->painted_bounds);

	if (server->fullscreen_window == window) {
		server->fullscreen_window = NULL;
		server->deferred_focus_id = 0;
	} else if (server->deferred_focus_id == window->window_id) {
		server->deferred_focus_id = 0;
	}

	while (*link != NULL && *link != window)
		link = &(*link)->next;
	if (*link == window)
		*link = window->next;
	link = &client->windows;
	while (*link != NULL && *link != window)
		link = &(*link)->owner_next;
	if (*link == window)
		*link = window->owner_next;
	client_discard_staging(window);
	client_discard_reservation(window);
	client_discard_committed(window);
	if (window->list_pending == GUI_PROTOCOL_WINDOW_LIST_REMOVE) {
		client->record_bytes -= sizeof(*window);
		window->client = NULL;
		window->next = server->removed_windows;
		server->removed_windows = window;
		return;
	}
	if (window->list_pending != 0)
		server->list_pending--;
	wm_record_destroy(server, client, window, sizeof(*window));
}

static void wm_discard_shell_list(wm_server *server)
{
	for (wm_window *window = server->windows; window != NULL; window = window->next) {
		window->list_visible = 0;
		window->list_pending = 0;
	}
	while (server->removed_windows != NULL) {
		wm_window *window = server->removed_windows;

		server->removed_windows = window->next;
		wm_record_destroy(server, NULL, window, sizeof(*window));
	}
	server->list_pending = 0;
}

static void client_destroy(wm_client *client)
{
	wm_server *server = client->server;
	wm_client **link = &server->clients;

	while (client->windows != NULL)
		window_destroy(client->windows);
	for (wm_launch *launch = server->launches; launch != NULL; launch = launch->next) {
		if (launch->client == client) {
			client->record_bytes -= sizeof(*launch);
			launch->client = NULL;
		}
	}
	while (*link != NULL && *link != client)
		link = &(*link)->next;
	if (*link == client) {
		*link = client->next;
		server->client_count--;
	}
	if (client->fd >= 0)
		close(client->fd);
	gui_protocol_input_destroy(client->input);
	gui_protocol_output_destroy(client->output);
	wm_record_destroy(server, client, client, sizeof(*client));
}

static int wm_load_cursor(wm_state *state)
{
	uint8_t *bytes = state->cursor_rgba;
	uint8_t extra;
	size_t remaining = sizeof(state->cursor_rgba);
	int fd = open(WM_CURSOR_PATH, O_RDONLY);

	state->cursor_loaded = 0;
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
	state->cursor_loaded = 1;
	return 0;
}

static const uint16_t wm_cursor_masks[WM_CURSOR_SHAPE_COUNT][WM_CURSOR_HEIGHT] = {
	[WM_CURSOR_RESIZE_HORIZONTAL] = {
		0x0000, 0x0000, 0x0000, 0x0000,
		0x0220, 0x0630, 0x0e38, 0x1ffc,
		0x0e38, 0x0630, 0x0220, 0x0000,
		0x0000, 0x0000, 0x0000, 0x0000,
	},
	[WM_CURSOR_RESIZE_VERTICAL] = {
		0x0000, 0x0000, 0x0080, 0x01c0,
		0x03e0, 0x07f0, 0x0080, 0x0080,
		0x0080, 0x07f0, 0x03e0, 0x01c0,
		0x0080, 0x0000, 0x0000, 0x0000,
	},
	[WM_CURSOR_RESIZE_NW_SE] = {
		0x0000, 0x0000, 0x007c, 0x003c,
		0x001c, 0x002c, 0x0044, 0x0080,
		0x1100, 0x1a00, 0x1c00, 0x1e00,
		0x1f00, 0x0000, 0x0000, 0x0000,
	},
	[WM_CURSOR_RESIZE_NE_SW] = {
		0x0000, 0x0000, 0x1f00, 0x1e00,
		0x1c00, 0x1a00, 0x1100, 0x0080,
		0x0044, 0x002c, 0x001c, 0x003c,
		0x007c, 0x0000, 0x0000, 0x0000,
	},
	[WM_CURSOR_MOVE] = {
		0x0000, 0x0000, 0x0080, 0x01c0,
		0x0080, 0x0080, 0x0888, 0x1ffc,
		0x0888, 0x0080, 0x0080, 0x01c0,
		0x0080, 0x0000, 0x0000, 0x0000,
	},
};

static gui_rect wm_cursor_bounds(wm_cursor_shape shape, uint32_t x, uint32_t y)
{
	int32_t hotspot = shape == WM_CURSOR_ARROW ? 0 : 7;

	return (gui_rect) { (int32_t)x - hotspot, (int32_t)y - hotspot,
		WM_CURSOR_WIDTH, WM_CURSOR_HEIGHT };
}

static void wm_draw_cursor(wm_state *state, gui_rect clip)
{
	gui_rect bounds = wm_cursor_bounds(state->cursor_shape, state->cursor_x, state->cursor_y);
	const uint16_t *masks = wm_cursor_masks[state->cursor_shape];

	if (!state->cursor_loaded)
		return;
	for (uint32_t row = 0; row < WM_CURSOR_HEIGHT; row++) {
		int32_t y = bounds.y + (int32_t)row;
		uint16_t foreground = masks[row];
		uint16_t outline = foreground;

		if (y >= (int64_t)state->height)
			break;
		if (y < 0 || y < clip.y || y >= clip.y + clip.h)
			continue;
		if (row > 0)
			outline |= masks[row - 1];
		if (row + 1 < WM_CURSOR_HEIGHT)
			outline |= masks[row + 1];
		outline |= (outline << 1) | (outline >> 1);
		for (uint32_t column = 0; column < WM_CURSOR_WIDTH; column++) {
			int32_t x = bounds.x + (int32_t)column;
			size_t source = ((size_t)row * WM_CURSOR_WIDTH + column) * 4;
			uint32_t color;

			if (x >= (int64_t)state->width)
				break;
			if (x < 0 || x < clip.x || x >= clip.x + clip.w)
				continue;
			if (state->cursor_shape == WM_CURSOR_ARROW) {
				if (state->cursor_rgba[source + 3] == 0)
					continue;
				color = (uint32_t)state->cursor_rgba[source] << 16 |
					(uint32_t)state->cursor_rgba[source + 1] << 8 |
					state->cursor_rgba[source + 2];
			} else {
				if (!(outline & (1U << column)))
					continue;
				color = foreground & (1U << column) ?
					WM_WINDOW_CONTENT_COLOR : WM_WINDOW_BORDER_COLOR;
			}
			state->composite->pixels[(size_t)y * state->composite->stride + x] = color;
		}
	}
}

static const wm_window *wm_presented_window(const wm_window *window, wm_window *storage)
{
	if (!window->mode_pending)
		return window;
	*storage = *window;
	storage->x = window->previous_x;
	storage->y = window->previous_y;
	storage->width = window->previous_width;
	storage->height = window->previous_height;
	storage->fullscreen = window->previous_fullscreen;
	return storage;
}

static wm_window *wm_presented_fullscreen(const wm_server *server)
{
	wm_window *window = server->fullscreen_window;

	return window != NULL && (window->mode_pending ? window->previous_fullscreen :
		window->fullscreen) ? window : NULL;
}

static int wm_title_button_x(const wm_window *client, wm_title_button button)
{
	if (button == WM_TITLE_BUTTON_CLOSE)
		return (int)client->x + client->width - WM_TITLE_BUTTON_WIDTH;
	if (button == WM_TITLE_BUTTON_MAXIMIZE)
		return (int)client->x + client->width - 2 * WM_TITLE_BUTTON_WIDTH;
	return (int)client->x + client->width - 3 * WM_TITLE_BUTTON_WIDTH;
}

static gui_color wm_title_button_background(const wm_window *client,
					    wm_title_button button)
{
	if (client->pressed_button == button)
		return WM_WINDOW_BORDER_COLOR;
	if (client->hover_button == button)
		return WM_WINDOW_CONTENT_COLOR;
	return client->focused ? WM_WINDOW_TITLE_ACTIVE_COLOR :
		WM_WINDOW_TITLE_INACTIVE_COLOR;
}

static gui_color wm_title_icon_color(const wm_window *client,
				     wm_title_button button)
{
	return client->pressed_button == button ? WM_WINDOW_CONTENT_COLOR :
		WM_WINDOW_BORDER_COLOR;
}

static void wm_draw_title_icon(wm_state *state, int button_x, int button_y,
			       wm_title_button button, gui_color color)
{
	int icon_x = button_x + 4;
	int icon_y = button_y + 8;

	if (button == WM_TITLE_BUTTON_MINIMIZE) {
		gui_fill_rect(state->composite, (gui_rect) { icon_x, icon_y + 6, 8, 2 },
			color);
		return;
	}
	if (button == WM_TITLE_BUTTON_MAXIMIZE) {
		gui_fill_rect(state->composite, (gui_rect) { icon_x, icon_y, 8, 1 }, color);
		gui_fill_rect(state->composite, (gui_rect) { icon_x, icon_y + 7, 8, 1 }, color);
		gui_fill_rect(state->composite, (gui_rect) { icon_x, icon_y, 1, 8 }, color);
		gui_fill_rect(state->composite, (gui_rect) { icon_x + 7, icon_y, 1, 8 }, color);
		return;
	}
	if (button == WM_TITLE_BUTTON_CLOSE) {
		for (int offset = 0; offset < 8; offset++) {
			gui_fill_rect(state->composite, (gui_rect) { icon_x + offset,
				icon_y + offset, 1, 1 }, color);
			gui_fill_rect(state->composite, (gui_rect) { icon_x + 7 - offset,
				icon_y + offset, 1, 1 }, color);
		}
	}
}

static void wm_draw_window(wm_state *state, const wm_window *client)
{
	wm_window presented;
	uint32_t committed_width = 0;
	uint32_t committed_height = 0;

	client = wm_presented_window(client, &presented);
	int outer_x = client->x - (int)WM_RESIZE_GRIP;
	int outer_y = client->y - (int)WM_TITLE_HEIGHT - (int)WM_RESIZE_GRIP;

	/* Content coordinates remain client-visible; only the server owns chrome. */
	if (!client->fullscreen) {
		gui_fill_rect(state->composite, (gui_rect) { .x = outer_x, .y = outer_y,
			.w = (int)client->width + 2 * WM_RESIZE_GRIP,
			.h = (int)client->height + WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP },
			WM_WINDOW_BORDER_COLOR);
		gui_fill_rect(state->composite, (gui_rect) { .x = client->x,
			.y = client->y - WM_TITLE_HEIGHT, .w = (int)client->width,
			.h = WM_TITLE_HEIGHT }, client->focused ? WM_WINDOW_TITLE_ACTIVE_COLOR :
			WM_WINDOW_TITLE_INACTIVE_COLOR);
		for (wm_title_button button = WM_TITLE_BUTTON_MINIMIZE;
		     button <= WM_TITLE_BUTTON_CLOSE; button++) {
			int button_x = wm_title_button_x(client, button);
			int button_y = client->y - WM_TITLE_HEIGHT;

			gui_fill_rect(state->composite, (gui_rect) { button_x, button_y,
				WM_TITLE_BUTTON_WIDTH, WM_TITLE_HEIGHT },
				wm_title_button_background(client, button));
			wm_draw_title_icon(state, button_x, button_y, button,
				wm_title_icon_color(client, button));
		}
	}
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

static gui_rect wm_window_bounds(const wm_server *server, const wm_window *window)
{
	wm_window presented;
	int64_t left = 0;
	int64_t top = 0;
	int64_t right = server->state.width;
	int64_t bottom = server->state.height;

	if (window != NULL)
		window = wm_presented_window(window, &presented);
	if (window != NULL && window->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
	    !window->fullscreen) {
		left = (int64_t)window->x - WM_RESIZE_GRIP;
		top = (int64_t)window->y - WM_TITLE_HEIGHT - WM_RESIZE_GRIP;
		right = (int64_t)window->x + window->width + WM_RESIZE_GRIP;
		bottom = (int64_t)window->y + window->height + WM_RESIZE_GRIP;
	}
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	if (right > server->state.width)
		right = server->state.width;
	if (bottom > server->state.height)
		bottom = server->state.height;
	return (gui_rect) { .x = (int)left, .y = (int)top,
		.w = right > left ? (int)(right - left) : 0,
		.h = bottom > top ? (int)(bottom - top) : 0 };
}

static int wm_window_above(const wm_window *cover, const wm_window *window)
{
	wm_window *fullscreen = wm_presented_fullscreen(cover->server);

	return cover != window && cover->client->fd >= 0 && cover->window_id != 0 &&
		cover->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY && !cover->minimized &&
		(fullscreen != NULL ? cover == fullscreen :
		 (window == NULL || window->client->role == GUI_PROTOCOL_ENDPOINT_SHELL ||
		  cover->stack_order > window->stack_order));
}

static int wm_window_obscured(const wm_server *server, const wm_window *window)
{
	gui_rect target = wm_window_bounds(server, window);
	int bottom = target.y + target.h;
	int right = target.x + target.w;

	for (int top = target.y; top < bottom;) {
		int band_bottom = bottom;
		int covered = target.x;

		for (wm_window *cover = server->windows; cover != NULL; cover = cover->next) {
			gui_rect rectangle;

			if (!wm_window_above(cover, window))
				continue;
			rectangle = wm_window_bounds(server, cover);
			if (rectangle.w == 0 || rectangle.h == 0 ||
			    rectangle.x >= right || rectangle.x + rectangle.w <= target.x)
				continue;
			if (rectangle.y > top && rectangle.y < band_bottom)
				band_bottom = rectangle.y;
			if (rectangle.y + rectangle.h > top &&
			    rectangle.y + rectangle.h < band_bottom)
				band_bottom = rectangle.y + rectangle.h;
		}
		while (covered < right) {
			int previous = covered;

			for (wm_window *cover = server->windows; cover != NULL; cover = cover->next) {
				gui_rect rectangle;

				if (!wm_window_above(cover, window))
					continue;
				rectangle = wm_window_bounds(server, cover);
				if (rectangle.y <= top && rectangle.y + rectangle.h >= band_bottom &&
				    rectangle.x <= covered && rectangle.x + rectangle.w > covered)
					covered = rectangle.x + rectangle.w;
			}
			if (covered == previous)
				return 0;
		}
		top = band_bottom;
	}
	return 1;
}

static gui_surface *wm_desktop_surface(const wm_server *server)
{
	gui_surface *desktop = server->desktop_snapshot;

	for (wm_window *client = server->windows; client != NULL; client = client->next) {

		if (client->client->fd >= 0 && client->client->role == GUI_PROTOCOL_ENDPOINT_SHELL &&
		    client->committed != NULL)
			desktop = client->committed;
	}
	return desktop;
}

static int wm_window_visible(const wm_server *server, const wm_window *window)
{
	wm_window *fullscreen = wm_presented_fullscreen(server);

	return window->client->fd >= 0 && window->window_id != 0 &&
		window->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY && !window->minimized &&
		(fullscreen == NULL || fullscreen == window) && !wm_window_obscured(server, window);
}

static const void *wm_desktop_identity(const wm_server *server)
{
	const void *identity = server->desktop_snapshot;

	for (wm_window *window = server->windows; window != NULL; window = window->next)
		if (window->client->fd >= 0 && window->window_id != 0 &&
		    window->client->role == GUI_PROTOCOL_ENDPOINT_SHELL && window->committed != NULL)
			identity = window;
	return identity;
}

static int wm_same_rect(gui_rect first, gui_rect second)
{
	return first.x == second.x && first.y == second.y && first.w == second.w && first.h == second.h;
}

static gui_rect wm_content_bounds(const wm_window *window)
{
	wm_window storage;
	const wm_window *presented = wm_presented_window(window, &storage);

	return (gui_rect) { presented->x, presented->y, presented->width, presented->height };
}

static void wm_collect_damage(wm_server *server)
{
	wm_state *state = &server->state;
	wm_window *fullscreen = wm_presented_fullscreen(server);
	uint32_t fullscreen_id = fullscreen != NULL ? fullscreen->window_id : 0;

	wm_update_cursor(server);
	wm_damage_init(state);
	if (!state->painted || state->painted_fullscreen != fullscreen_id ||
	    (state->painted_desktop != wm_desktop_identity(server) && !wm_window_obscured(server, NULL)))
		gui_region_mark_full(&state->damage);
	if (state->painted && (state->cursor_x != state->painted_cursor_x ||
	    state->cursor_y != state->painted_cursor_y ||
	    state->cursor_shape != state->painted_cursor_shape ||
	    state->cursor_loaded != state->painted_cursor_loaded)) {
		wm_damage_rect(state, wm_cursor_bounds(state->painted_cursor_shape,
			state->painted_cursor_x, state->painted_cursor_y));
		wm_damage_rect(state, wm_cursor_bounds(state->cursor_shape,
			state->cursor_x, state->cursor_y));
	}
	for (wm_window *window = server->windows; window != NULL; window = window->next) {
		int visible = wm_window_visible(server, window);
		gui_rect bounds = wm_window_bounds(server, window);

		if (window->painted == visible && (!visible ||
		    (wm_same_rect(bounds, window->painted_bounds) &&
		     wm_same_rect(wm_content_bounds(window), window->painted_content) &&
		     (window == fullscreen || (window->focused == window->painted_focus &&
		      window->stack_order == window->painted_order &&
		      window->hover_button == window->painted_hover_button &&
		      window->pressed_button == window->painted_pressed_button)))))
			continue;
		if (window->painted)
			wm_damage_rect(state, window->painted_bounds);
		if (visible)
			wm_damage_rect(state, bounds);
	}
}

static void wm_record_presented(wm_server *server)
{
	wm_state *state = &server->state;
	wm_window *fullscreen = wm_presented_fullscreen(server);

	state->painted = 1;
	state->painted_cursor_x = state->cursor_x;
	state->painted_cursor_y = state->cursor_y;
	state->painted_cursor_shape = state->cursor_shape;
	state->painted_cursor_loaded = state->cursor_loaded;
	state->painted_fullscreen = fullscreen != NULL ? fullscreen->window_id : 0;
	state->painted_desktop = wm_desktop_identity(server);
	for (wm_window *window = server->windows; window != NULL; window = window->next) {
		window->painted = wm_window_visible(server, window);
		window->painted_bounds = wm_window_bounds(server, window);
		window->painted_content = wm_content_bounds(window);
		window->painted_focus = window->focused;
		window->painted_hover_button = window->hover_button;
		window->painted_pressed_button = window->pressed_button;
		window->painted_order = window->stack_order;
	}
}

static void wm_paint_rect(wm_server *server, gui_rect rectangle)
{
	wm_state clipped = server->state;
	gui_surface view = { .width = rectangle.w, .height = rectangle.h,
		.stride = server->state.composite->stride,
		.pixels = server->state.composite->pixels +
			(size_t)rectangle.y * server->state.composite->stride + rectangle.x };
	wm_state *state = &clipped;
	wm_window *fullscreen = wm_presented_fullscreen(server);
	gui_surface *desktop = wm_desktop_surface(server);

	state->composite = &view;
	gui_fill_rect(state->composite, (gui_rect) { .x = 0, .y = 0,
		.w = state->composite->width, .h = state->composite->height }, 0);
	if (desktop != NULL && !wm_window_obscured(server, NULL)) {
		gui_blit(state->composite, -rectangle.x, -rectangle.y, desktop,
			&(gui_rect) { .x = 0, .y = 0, .w = desktop->width,
				.h = desktop->height });
	}
	/* Select the next layer by serial so a freed client slot cannot reorder windows. */
	for (uint64_t painted = 0;;) {
		wm_window *next = NULL;

		for (wm_window *client = server->windows; client != NULL; client = client->next) {

			if (client->client->fd >= 0 && client->window_id != 0 &&
			    client->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
			    !client->minimized &&
			    (fullscreen == NULL || client == fullscreen) &&
			    client->stack_order > painted &&
			    (next == NULL || client->stack_order < next->stack_order))
				next = client;
		}
		if (next == NULL)
			break;
		if (!wm_window_obscured(server, next)) {
			wm_window presented;
			wm_window shifted = *wm_presented_window(next, &presented);

			shifted.mode_pending = 0;
			shifted.x -= rectangle.x;
			shifted.y -= rectangle.y;
			wm_draw_window(state, &shifted);
		}
		painted = next->stack_order;
	}
	/* The server-owned sprite is always the final layer, independent of focus. */
	wm_draw_cursor(&server->state, rectangle);
}

static gui_flush_result wm_compose(wm_server *server)
{
	wm_state *state = &server->state;

	if (!state->graphics)
		return GUI_FLUSH_COMPLETE;
	wm_collect_damage(server);
	if (state->damage.count == 0) {
		state->motion_pending = 0;
		return GUI_FLUSH_COMPLETE;
	}
	for (int index = 0; index < state->damage.count; index++) {
		gui_region_rect rectangle = state->damage.rects[index];

		wm_paint_rect(server, (gui_rect) { rectangle.x, rectangle.y, rectangle.width, rectangle.height });
	}
	for (int index = 0; index < state->damage.count; index++) {
		gui_region_rect rectangle = state->damage.rects[index];
		gui_flush_result result = gui_surface_flush_rect_fd(state->composite,
			(gui_rect) { rectangle.x, rectangle.y, rectangle.width, rectangle.height },
			state->scanout_stride, state->framebuffer);

		if (result != GUI_FLUSH_COMPLETE) {
			if (result == GUI_FLUSH_BUSY)
				state->scanout_busy = 1;
			if (result == GUI_FLUSH_ERROR)
				fprintf(stderr, "mario-wm: scanout failed: %d\n", errno);
			return result;
		}
	}
	if (wm_publish_completed_frame(state) < 0)
		return GUI_FLUSH_ERROR;
	wm_record_presented(server);
	gui_region_clear(&state->damage);
	state->motion_pending = 0;
	state->scanout_busy = 0;
	return GUI_FLUSH_COMPLETE;
}

static uint64_t wm_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		stop_requested = 1;
		return 0;
	}
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wm_motion_timeout(wm_server *server)
{
	wm_state *state = &server->state;
	uint64_t now;

	if (!state->motion_pending || !state->graphics || state->scanout_busy)
		return -1;
	now = wm_now_ms();
	return now >= state->motion_deadline_ms ? 0 :
		(int)(state->motion_deadline_ms - now);
}

static int wm_present_motion(wm_server *server)
{
	if (wm_motion_timeout(server) != 0)
		return 0;
	return wm_compose(server) == GUI_FLUSH_ERROR ? -1 : 0;
}

static void wm_present_input(wm_server *server, int changed, int motion_only)
{
	wm_state *state = &server->state;

	changed |= wm_update_cursor(server);
	if (motion_only) {
		if (changed && !state->motion_pending) {
			state->motion_pending = 1;
			state->motion_deadline_ms = wm_now_ms() + WM_MOTION_FRAME_MS;
		}
	} else if ((changed || state->motion_pending) && wm_compose(server) == GUI_FLUSH_ERROR) {
		stop_requested = 1;
	}
}

static wm_client *wm_add_client(wm_server *server, int fd,
				gui_protocol_endpoint_role role)
{
	wm_client *client;

	if (mark_close_on_exec(fd) < 0) {
		close(fd);
		return NULL;
	}
	client = wm_record_create(server, NULL, sizeof(*client));
	if (client == NULL) {
		close(fd);
		return NULL;
	}
	*client = (wm_client) { .fd = fd, .role = role,
		.input = gui_protocol_input_create(),
		.output = gui_protocol_output_create(GUI_PROTOCOL_OUTPUT_LIMIT),
		.server = server, .record_bytes = sizeof(*client) +
			sizeof(struct pollfd) + sizeof(wm_client *) };
	if (client->input == NULL || client->output == NULL ||
	    wm_poll_reserve(server, server->client_count + 1) < 0) {
		client_destroy(client);
		return NULL;
	}
	client->next = server->clients;
	server->clients = client;
	server->client_count++;
	return client;
}

static void wm_place_window(wm_server *server, wm_window *client)
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
	uint32_t maximum = server->state.width > 2 * WM_RESIZE_GRIP ?
		server->state.width - 2 * WM_RESIZE_GRIP : 1;

	return maximum < WM_MIN_CONTENT_WIDTH ? maximum : WM_MIN_CONTENT_WIDTH;
}

static uint32_t wm_minimum_height(const wm_server *server)
{
	uint32_t chrome_height = WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP;
	uint32_t maximum = server->state.height > chrome_height ?
		server->state.height - chrome_height : 1;

	return maximum < WM_MIN_CONTENT_HEIGHT ? maximum : WM_MIN_CONTENT_HEIGHT;
}

static int32_t wm_clamp_position(int64_t value, uint32_t size, uint32_t screen_size,
				 uint32_t leading_chrome)
{
	int64_t minimum = leading_chrome;
	int64_t maximum = screen_size > size + WM_RESIZE_GRIP ?
		(int64_t)screen_size - size - WM_RESIZE_GRIP : minimum;

	if (value < minimum)
		return (int32_t)minimum;
	if (value > maximum)
		return (int32_t)maximum;
	return (int32_t)value;
}

static void wm_clamp_window_position(wm_server *server, wm_window *client)
{
	client->x = wm_clamp_position(client->x, client->width, server->state.width,
		WM_RESIZE_GRIP);
	client->y = wm_clamp_position(client->y, client->height, server->state.height,
		WM_TITLE_HEIGHT + WM_RESIZE_GRIP);
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
	    fix.line_length < line_bytes)
		return -1;
	state->width = info.xres;
	state->height = info.yres;
	state->scanout_stride = fix.line_length;
	state->surface_limit = app_gui_surface_budget(state->frame_bytes);
	return state->frame_bytes <= state->surface_limit / 2 ? 0 : -1;
}

static int client_surface_fits(wm_window *client, size_t bytes, size_t released)
{
	wm_server *server = client->server;
	wm_state *state = &server->state;
	size_t headroom = state->frame_bytes;
	size_t used;

	if (bytes > state->frame_bytes || released > state->surface_bytes)
		return 0;
	used = state->surface_bytes - released;
	if (used > state->surface_limit || bytes > state->surface_limit - used)
		return 0;
	if (client->client->role == GUI_PROTOCOL_ENDPOINT_SHELL)
		return 1;
	if (released > client->client->surface_bytes ||
	    client->client->surface_bytes - released > app_gui_peer_surface_budget(state->frame_bytes) ||
	    bytes > app_gui_peer_surface_budget(state->frame_bytes) -
		(client->client->surface_bytes - released))
		return 0;
	for (wm_window *shell = server->windows; shell != NULL; shell = shell->next) {
		size_t replacement;

		if (shell->client->fd < 0 || shell->client->role != GUI_PROTOCOL_ENDPOINT_SHELL)
			continue;
		replacement = shell->staging_bytes + shell->reserved_bytes;
		headroom = replacement >= headroom ? 0 : headroom - replacement;
	}
	return headroom <= state->surface_limit - used - bytes;
}

static int client_reserve_surface(wm_window *client, size_t bytes)
{
	size_t released = client->staging_bytes + client->reserved_bytes;

	if (!client_surface_fits(client, bytes, released))
		return -ENOMEM;
	client_discard_staging(client);
	client_discard_reservation(client);
	client->reserved_bytes = bytes;
	client->server->state.surface_bytes += bytes;
	client->client->surface_bytes += bytes;
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

static int window_enqueue(wm_window *window, gui_protocol_type type,
			  const void *payload, size_t payload_bytes)
{
	return client_enqueue(window->client, type, payload, payload_bytes);
}

static void wm_reap_children(wm_server *server)
{
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		if (pid == server->shell_pid) {
			server->shell_pid = 0;
			continue;
		}
		for (wm_launch **link = &server->launches; *link != NULL;
		     link = &(*link)->next) {
			wm_launch *launch = *link;
			uint8_t message[sizeof(gui_protocol_wire_app_exit)];
			wm_window *window;

			if (launch->pid != pid)
				continue;
			window = wm_find_window(server, launch->origin_window_id);
			if (window != NULL && window->client->hello > 0) {
				put_u32(message, window->window_id);
				put_u32(message + 4, (uint32_t)(WIFEXITED(status) ?
					WEXITSTATUS(status) : -EINTR));
				if (window_enqueue(window, GUI_PROTOCOL_APP_EXIT, message,
					   sizeof(message)) < 0)
					window->client->hello = -1;
			}
			*link = launch->next;
			wm_record_destroy(server, launch->client, launch, sizeof(*launch));
			break;
		}
	}
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

static void window_reject(wm_window *window, gui_protocol_type type, int status)
{
	client_reject(window->client, type, status);
}

static void client_handle_launch_request(wm_client *client, const uint8_t *bytes,
					 size_t payload_bytes)
{
	uint32_t origin_window_id = client->windows == NULL ? 0 : client->windows->window_id;
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
	if (client->role != GUI_PROTOCOL_ENDPOINT_SHELL &&
	    (launch_type == GUI_PROTOCOL_LAUNCH_TERMINAL ||
	     client->windows == NULL)) {
		client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EPERM);
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
	case GUI_PROTOCOL_LAUNCH_OPEN_TEXT:
		if (path_bytes == 0 || bytes[sizeof(gui_protocol_wire_launch_request)] != '/' ||
		    memchr(bytes + sizeof(gui_protocol_wire_launch_request), '\0', path_bytes) != NULL) {
			client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
			return;
		}
		if (wm_start_terminal_command(bytes + sizeof(gui_protocol_wire_launch_request),
			path_bytes) < 0) {
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
	case GUI_PROTOCOL_LAUNCH_EXECUTABLE:
		if (path_bytes == 0 || bytes[sizeof(gui_protocol_wire_launch_request)] != '/' ||
		    memchr(bytes + sizeof(gui_protocol_wire_launch_request), '\0', path_bytes) != NULL) {
			client_reject(client, GUI_PROTOCOL_LAUNCH_REQUEST, -EINVAL);
			return;
		}
		if (wm_start_terminal_executable(client->server,
			bytes + sizeof(gui_protocol_wire_launch_request), path_bytes,
			origin_window_id) < 0) {
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

static int wm_enqueue_window_list(wm_client *shell, gui_protocol_type type,
				  const wm_window *window)
{
	uint8_t message[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES] = { 0 };

	put_u32(message, window->window_id);
	put_u32(message + 4, window->x);
	put_u32(message + 8, window->y);
	put_u32(message + 12, window->width);
	put_u32(message + 16, window->height);
	put_u32(message + 20, window->focused);
	put_u32(message + 24, window->stack_order);
	put_u16(message + 28, window->title_bytes);
	memcpy(message + sizeof(gui_protocol_wire_window_list), window->title, window->title_bytes);
	return client_enqueue(shell, type, message,
		sizeof(gui_protocol_wire_window_list) + window->title_bytes);
}

static void wm_notify_shell_window(wm_server *server, gui_protocol_type type,
				   wm_window *window)
{
	if (window->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY || window->window_id == 0)
		return;
	for (wm_client *shell = server->clients; shell != NULL; shell = shell->next) {
		if (shell->role != GUI_PROTOCOL_ENDPOINT_SHELL || shell->hello <= 0 ||
		    shell->windows == NULL)
			continue;
		if (!window->list_visible) {
			if (type == GUI_PROTOCOL_WINDOW_LIST_REMOVE) {
				if (window->list_pending != 0)
					server->list_pending--;
				window->list_pending = 0;
				return;
			}
			if (shell->snapshot_pending && window->window_id <= shell->snapshot_end)
				return;
			type = GUI_PROTOCOL_WINDOW_LIST_ADD;
		} else if (type != GUI_PROTOCOL_WINDOW_LIST_REMOVE) {
			type = GUI_PROTOCOL_WINDOW_LIST_CHANGE;
		}
		if (window->list_pending == 0)
			server->list_pending++;
		window->list_pending = type;
		return;
	}
}

static void wm_pump_shell_snapshot(wm_server *server, wm_client *shell)
{
	if (shell->role != GUI_PROTOCOL_ENDPOINT_SHELL || shell->windows == NULL || shell->hello <= 0 ||
	    gui_protocol_output_pending(shell->output))
		return;
	for (unsigned int count = 0; count < WM_SHELL_SNAPSHOT_QUANTUM; count++) {
		wm_window *next = NULL;
		gui_protocol_type type;

		if (server->removed_windows != NULL) {
			next = server->removed_windows;
			if (wm_enqueue_window_list(shell, GUI_PROTOCOL_WINDOW_LIST_REMOVE, next) < 0) {
				shell->hello = -1;
				return;
			}
			server->removed_windows = next->next;
			server->list_pending--;
			wm_record_destroy(server, NULL, next, sizeof(*next));
			continue;
		}
		if (shell->snapshot_pending) {
			for (wm_window *window = server->windows; window != NULL; window = window->next) {
				if (window->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
				    window->client->hello > 0 && window->window_id > shell->snapshot_cursor &&
				    window->window_id <= shell->snapshot_end &&
				    (next == NULL || window->window_id < next->window_id))
					next = window;
			}
			if (next == NULL)
				shell->snapshot_pending = 0;
		}
		if (next != NULL) {
			type = GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT;
		} else {
			for (unsigned int pass = 0; pass < 2 && next == NULL; pass++) {
				for (wm_window *window = server->windows; window != NULL; window = window->next) {
					if (window->list_pending != 0 &&
					    (pass != 0 || window->window_id > shell->delta_cursor) &&
					    (next == NULL || window->window_id < next->window_id))
						next = window;
				}
			}
			if (next == NULL)
				return;
			type = next->list_pending;
		}
		if (wm_enqueue_window_list(shell, type, next) < 0) {
			shell->hello = -1;
			return;
		}
		if (type == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT)
			shell->snapshot_cursor = next->window_id;
		else
			shell->delta_cursor = next->window_id;
		next->list_visible = 1;
		if (next->list_pending != 0)
			server->list_pending--;
		next->list_pending = 0;
	}
}

static void wm_send_shell_snapshot(wm_server *server, wm_client *shell)
{
	wm_discard_shell_list(server);
	shell->snapshot_cursor = 0;
	shell->snapshot_end = server->next_window_id;
	shell->delta_cursor = 0;
	shell->snapshot_pending = 1;
	wm_pump_shell_snapshot(server, shell);
}

static void client_fail_surface(wm_window *client, gui_protocol_type failed_type,
				int status)
{
	int mode_pending = client->mode_pending;

	client_discard_staging(client);
	client_discard_reservation(client);
	client->state_pending = 0;
	if (client->transition_pending) {
		client->x = client->previous_x;
		client->y = client->previous_y;
		client->width = client->previous_width;
		client->height = client->previous_height;
		client->transition_pending = 0;
		if (mode_pending) {
			client->fullscreen = client->previous_fullscreen;
			client->mode_pending = 0;
			client->server->fullscreen_window = client->fullscreen ? client : NULL;
			client->server->deferred_focus_id = 0;
			wm_reset_input_owner(client->server);
		}
		if (client->server->capture_client == client)
			wm_cancel_capture(client->server);
		wm_notify_shell_window(client->server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
	}
	window_reject(client, failed_type, status);
	if (client->configure_serial == UINT64_MAX) {
		client->client->hello = -1;
		return;
	}
	client->configure_serial++;
	if (client_enqueue_configure(client) < 0)
		client->client->hello = -1;
	if (mode_pending || !wm_window_obscured(client->server, client))
		(void)wm_compose(client->server);
}

static int client_stage_surface(wm_window *client, const uint8_t *bytes)
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
		window_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -EINVAL);
		return 0;
	}
	if (client->staging != NULL) {
		window_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -EBUSY);
		return 0;
	}
	if ((client->client->capabilities & GUI_PROTOCOL_CAP_SURFACE_PATCH) &&
	    frame_serial <= client->last_admitted_frame_serial) {
		window_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -ESTALE);
		return 0;
	}
	if (client->reserved_bytes != bytes_needed &&
	    !client_surface_fits(client, bytes_needed, client->reserved_bytes)) {
		window_reject(client, GUI_PROTOCOL_SURFACE_BEGIN, -ENOMEM);
		return 0;
	}
	surface = gui_surface_create((int)width, (int)height);
	if (surface == NULL) {
		client_fail_surface(client, GUI_PROTOCOL_SURFACE_BEGIN, -ENOMEM);
		return 0;
	}
	state->surface_bytes -= client->reserved_bytes;
	client->client->surface_bytes -= client->reserved_bytes;
	client->reserved_bytes = 0;
	client->staging = surface;
	client->staging_bytes = bytes_needed;
	client->staging_frame_serial = frame_serial;
	client->staging_pixels = 0;
	client->staging_patch = 0;
	client->staging_rectangle = (gui_rect) { 0, 0, width, height };
	if (frame_serial > client->last_admitted_frame_serial)
		client->last_admitted_frame_serial = frame_serial;
	state->surface_bytes += bytes_needed;
	client->client->surface_bytes += bytes_needed;
	return 0;
}

static int client_stage_patch(wm_window *window, const uint8_t *bytes)
{
	uint64_t configure_serial = get_u64(bytes + 4);
	uint64_t base_frame_serial = get_u64(bytes + 12);
	uint64_t frame_serial = get_u64(bytes + 20);
	int32_t rectangle_x = (int32_t)get_u32(bytes + 28);
	int32_t rectangle_y = (int32_t)get_u32(bytes + 32);
	uint32_t width = get_u32(bytes + 36);
	uint32_t height = get_u32(bytes + 40);
	size_t bytes_needed;
	gui_surface *surface;

	if (!(window->client->capabilities & GUI_PROTOCOL_CAP_SURFACE_PATCH)) {
		window_reject(window, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, -EPROTONOSUPPORT);
		return 0;
	}
	if (get_u32(bytes) != window->window_id || configure_serial != window->configure_serial ||
	    frame_serial == 0 || width == 0 || height == 0 || rectangle_x < 0 || rectangle_y < 0 ||
	    (uint32_t)rectangle_x > window->width || (uint32_t)rectangle_y > window->height ||
	    width > window->width - (uint32_t)rectangle_x || height > window->height - (uint32_t)rectangle_y ||
	    surface_bytes(window->width, window->height, &bytes_needed) < 0 ||
	    bytes_needed > window->server->state.frame_bytes) {
		window_reject(window, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, -EINVAL);
		return 0;
	}
	if (window->staging != NULL) {
		window_reject(window, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, -EBUSY);
		return 0;
	}
	if (window->committed == NULL || window->mode_pending ||
	    window->committed_configure_serial != configure_serial ||
	    base_frame_serial == 0 || base_frame_serial != window->committed_frame_serial ||
	    frame_serial <= window->last_admitted_frame_serial ||
	    window->committed->width != (int)window->width || window->committed->height != (int)window->height) {
		window_reject(window, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, -ESTALE);
		return 0;
	}
	if (!client_surface_fits(window, bytes_needed, 0)) {
		window_reject(window, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, -ENOMEM);
		return 0;
	}
	surface = gui_surface_create(window->width, window->height);
	if (surface == NULL) {
		client_fail_surface(window, GUI_PROTOCOL_SURFACE_PATCH_BEGIN, -ENOMEM);
		return 0;
	}
	memcpy(surface->pixels, window->committed->pixels, bytes_needed);
	window->staging = surface;
	window->staging_bytes = bytes_needed;
	window->staging_pixels = 0;
	window->staging_patch = 1;
	window->staging_rectangle = (gui_rect) { rectangle_x, rectangle_y, width, height };
	window->staging_base_frame_serial = base_frame_serial;
	window->staging_frame_serial = frame_serial;
	window->last_admitted_frame_serial = frame_serial;
	window->client->surface_bytes += bytes_needed;
	window->server->state.surface_bytes += bytes_needed;
	return 0;
}

static void client_cancel_surface(wm_window *window, const uint8_t *bytes)
{
	if (!(window->client->capabilities & GUI_PROTOCOL_CAP_SURFACE_PATCH)) {
		window_reject(window, GUI_PROTOCOL_SURFACE_CANCEL, -EPROTONOSUPPORT);
		return;
	}
	if (get_u32(bytes) != window->window_id || get_u64(bytes + 4) != window->configure_serial ||
	    window->staging == NULL || get_u64(bytes + 12) != window->staging_frame_serial) {
		window_reject(window, GUI_PROTOCOL_SURFACE_CANCEL, -EINVAL);
		return;
	}
	client_discard_staging(window);
}

static int client_surface_data(wm_window *client, const uint8_t *bytes,
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
	    (surface_pixels = client->staging_patch ?
		(size_t)client->staging_rectangle.w * client->staging_rectangle.h :
		(size_t)client->width * client->height) > UINT32_MAX ||
	    pixel_offset != client->staging_pixels || pixel_offset > surface_pixels ||
	    pixel_count > surface_pixels - pixel_offset ||
	    pixel_count > SIZE_MAX / sizeof(gui_color) ||
	    (byte_count = (size_t)pixel_count * sizeof(gui_color)) !=
		payload_bytes - sizeof(gui_protocol_wire_surface_data)) {
		window_reject(client, GUI_PROTOCOL_SURFACE_DATA, -EINVAL);
		return 0;
	}
	if (client->staging_patch) {
		size_t remaining = pixel_count;
		size_t offset = pixel_offset;
		const uint8_t *source = bytes + sizeof(gui_protocol_wire_surface_data);
		gui_rect rectangle = client->staging_rectangle;

		while (remaining > 0) {
			size_t column = offset % rectangle.w;
			size_t count = rectangle.w - column;
			size_t destination = (rectangle.y + offset / rectangle.w) * client->width + rectangle.x + column;

			if (count > remaining)
				count = remaining;
			memcpy(client->staging->pixels + destination, source, count * sizeof(gui_color));
			source += count * sizeof(gui_color);
			offset += count;
			remaining -= count;
		}
	} else {
		memcpy(client->staging->pixels + pixel_offset,
			bytes + sizeof(gui_protocol_wire_surface_data), byte_count);
	}
	client->staging_pixels += pixel_count;
	return window_enqueue(client, GUI_PROTOCOL_SURFACE_DATA_REPLY, bytes,
			      sizeof(gui_protocol_wire_surface_data));
}

static int client_commit_surface(wm_window *client, const uint8_t *bytes)
{
	uint32_t window_id = get_u32(bytes);
	uint64_t configure_serial = get_u64(bytes + 4);
	uint64_t frame_serial = get_u64(bytes + 12);
	gui_flush_result result;
	int mode_pending = client->mode_pending;

	if (client->staging == NULL || window_id != client->window_id ||
	    configure_serial != client->configure_serial ||
	    frame_serial != client->staging_frame_serial ||
	    client->staging_pixels != (client->staging_patch ?
		(size_t)client->staging_rectangle.w * client->staging_rectangle.h :
		(size_t)client->width * client->height) ||
	    (client->staging_patch && client->staging_base_frame_serial != client->committed_frame_serial)) {
		window_reject(client, GUI_PROTOCOL_SURFACE_COMMIT, -EINVAL);
		return 0;
	}
	{
		gui_surface *old = client->committed;
		size_t old_bytes = client->committed_bytes;

		client->committed = client->staging;
		client->committed_bytes = client->staging_bytes;
		client->staging = NULL;
		client->staging_bytes = 0;
		client->mode_pending = 0;
		if (mode_pending || !wm_window_obscured(client->server, client)) {
			if (client->staging_patch) {
				gui_rect rectangle = client->staging_rectangle;

				rectangle.x += client->x;
				rectangle.y += client->y;
				wm_damage_rect(&client->server->state, rectangle);
			} else if (client->client->role == GUI_PROTOCOL_ENDPOINT_SHELL)
				wm_damage_rect(&client->server->state,
					(gui_rect) { 0, 0, client->width, client->height });
			else
				wm_damage_rect(&client->server->state,
					(gui_rect) { client->x, client->y, client->width, client->height });
		}
		result = !mode_pending && wm_window_obscured(client->server, client) ? GUI_FLUSH_COMPLETE :
			wm_compose(client->server);
		if (result == GUI_FLUSH_ERROR) {
			client->mode_pending = mode_pending;
			client->staging = client->committed;
			client->staging_bytes = client->committed_bytes;
			client->committed = old;
			client->committed_bytes = old_bytes;
			client_fail_surface(client, GUI_PROTOCOL_SURFACE_COMMIT, -EIO);
			return 0;
		}
		if (old != NULL) {
			client->server->state.surface_bytes -= old_bytes;
			client->client->surface_bytes -= old_bytes;
			gui_surface_destroy(old);
		}
	}
	client->staging_frame_serial = 0;
	client->staging_pixels = 0;
	client->staging_patch = 0;
	client->committed_frame_serial = frame_serial;
	client->committed_configure_serial = configure_serial;
	client->transition_pending = 0;
	if (mode_pending)
		wm_complete_mode(client);
	if (client->state_pending) {
		uint32_t state = client->pending_state;

		client->state_pending = 0;
		if (state == GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED)
			client->maximized = 1;
		else if (state == GUI_PROTOCOL_WINDOW_STATE_NORMAL)
			client->maximized = 0;
		if (client_enqueue_window_state(client, state) < 0)
			client->client->hello = -1;
	}
	if (client->client->role == GUI_PROTOCOL_ENDPOINT_SHELL) {
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

static void wm_flush_client_outputs(wm_server *server)
{
	for (wm_client *client = server->clients; client != NULL; client = client->next) {

		if (client->fd >= 0 && client_flush(client) < 0)
			client->hello = -1;
	}
}

static int client_enqueue_focus(wm_window *client, int focused)
{
	uint8_t focus[sizeof(gui_protocol_wire_window_focus)];

	put_u32(focus, client->window_id);
	put_u32(focus + 4, focused);
	return window_enqueue(client, GUI_PROTOCOL_WINDOW_FOCUS, focus, sizeof(focus));
}

static int client_enqueue_configure(wm_window *client)
{
	uint8_t configure[sizeof(gui_protocol_wire_window_configure)];

	put_u32(configure, client->window_id);
	put_u64(configure + 4, client->configure_serial);
	put_u32(configure + 12, client->width);
	put_u32(configure + 16, client->height);
	return window_enqueue(client, GUI_PROTOCOL_WINDOW_CONFIGURE, configure,
		sizeof(configure));
}

static int client_enqueue_window_state(wm_window *client, uint32_t state)
{
	uint8_t message[sizeof(gui_protocol_wire_window_state)];

	put_u32(message, client->window_id);
	put_u32(message + 4, state);
	put_u64(message + 8, client->configure_serial);
	return window_enqueue(client, GUI_PROTOCOL_WINDOW_STATE, message,
		sizeof(message));
}

static void client_enqueue_close_request(wm_window *client)
{
	uint8_t close_request[sizeof(gui_protocol_wire_window_id)];

	if (client == NULL || client->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
	    client->window_id == 0)
		return;
	put_u32(close_request, client->window_id);
	if (window_enqueue(client, GUI_PROTOCOL_WINDOW_CLOSE_REQUEST, close_request,
			   sizeof(close_request)) < 0)
		client->client->hello = -1;
}

static int wm_resize_window(wm_server *server, wm_window *client, int32_t x,
			    int32_t y, uint32_t width, uint32_t height)
{
	int32_t previous_x = client->x;
	int32_t previous_y = client->y;
	uint32_t previous_width = client->width;
	uint32_t previous_height = client->height;
	uint32_t minimum_width = wm_minimum_width(server);
	uint32_t minimum_height = wm_minimum_height(server);
	uint32_t maximum_width = server->state.width > 2 * WM_RESIZE_GRIP ?
		server->state.width - 2 * WM_RESIZE_GRIP : minimum_width;
	uint32_t maximum_height = server->state.height >
		WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP ? server->state.height -
		(WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP) : minimum_height;
	uint32_t position_width_limit;
	uint32_t position_height_limit;
	size_t bytes_needed;
	wm_window proposed = *client;

	if (client->fullscreen || client->mode_pending)
		return -EBUSY;
	width = wm_clamp_dimension(width, minimum_width, maximum_width);
	height = wm_clamp_dimension(height, minimum_height, maximum_height);
	proposed.x = x;
	proposed.y = y;
	proposed.width = width;
	proposed.height = height;
	wm_clamp_window_position(server, &proposed);
	position_width_limit = server->state.width > (uint32_t)proposed.x +
		WM_RESIZE_GRIP ? server->state.width - proposed.x - WM_RESIZE_GRIP :
		minimum_width;
	position_height_limit = server->state.height > (uint32_t)proposed.y +
		WM_RESIZE_GRIP ? server->state.height - proposed.y - WM_RESIZE_GRIP :
		minimum_height;
	proposed.width = wm_clamp_dimension(proposed.width, minimum_width,
		position_width_limit);
	proposed.height = wm_clamp_dimension(proposed.height, minimum_height,
		position_height_limit);
	if (proposed.x == previous_x && proposed.y == previous_y &&
	    proposed.width == previous_width && proposed.height == previous_height)
		return 0;
	if (client->configure_serial >= UINT64_MAX - 1)
		return -EOVERFLOW;
	if (surface_bytes(proposed.width, proposed.height, &bytes_needed) < 0)
		return -EINVAL;
	if (client_reserve_surface(client, bytes_needed) < 0)
		return -ENOMEM;
	if (!client->transition_pending) {
		client->previous_x = previous_x;
		client->previous_y = previous_y;
		client->previous_width = previous_width;
		client->previous_height = previous_height;
		client->transition_pending = 1;
	}
	client->x = proposed.x;
	client->y = proposed.y;
	client->width = proposed.width;
	client->height = proposed.height;
	client->configure_serial++;
	if (client_enqueue_configure(client) < 0) {
		client_fail_surface(client, GUI_PROTOCOL_WINDOW_RESIZE, -EIO);
		client->client->hello = -1;
		return -EIO;
	}
	wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
	if (wm_compose(server) == GUI_FLUSH_ERROR) {
		client_fail_surface(client, GUI_PROTOCOL_WINDOW_RESIZE, -EIO);
		return 0;
	}
	return 1;
}

static int wm_maximize_geometry(wm_server *server, int32_t *x, int32_t *y,
				uint32_t *width, uint32_t *height)
{
	uint32_t minimum_width = wm_minimum_width(server);
	uint32_t minimum_height = wm_minimum_height(server);
	uint32_t chrome_height = WM_TITLE_HEIGHT + 2 * WM_RESIZE_GRIP;

	if (!server->work_area_valid)
		return -EAGAIN;
	*width = server->work_width > 2 * WM_RESIZE_GRIP ?
		server->work_width - 2 * WM_RESIZE_GRIP : minimum_width;
	*height = server->work_height > chrome_height ?
		server->work_height - chrome_height : minimum_height;
	*x = server->work_x + WM_RESIZE_GRIP;
	*y = server->work_y + WM_TITLE_HEIGHT + WM_RESIZE_GRIP;
	return 0;
}

static int wm_request_minimize(wm_server *server, wm_window *window)
{
	if (window == NULL || window->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
		return -EINVAL;
	if (window->fullscreen || window->mode_pending || window->transition_pending)
		return -EBUSY;
	if (window->minimized) {
		window->minimized = 0;
		if (client_enqueue_window_state(window, GUI_PROTOCOL_WINDOW_STATE_NORMAL) < 0)
			window->client->hello = -1;
		wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, window);
		if (server->input_owner == NULL)
			wm_set_input_owner(server, window);
		if (wm_compose(server) == GUI_FLUSH_ERROR)
			stop_requested = 1;
		return 1;
	}
	if (server->capture_client == window ||
	    server->title_click_window_id == window->window_id)
		wm_cancel_capture(server);
	if (server->input_owner == window)
		wm_set_input_owner(server, NULL);
	window->minimized = 1;
	if (client_enqueue_window_state(window, GUI_PROTOCOL_WINDOW_STATE_MINIMIZED) < 0)
		window->client->hello = -1;
	wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, window);
	if (server->input_owner == NULL)
		wm_set_input_owner(server, wm_next_focus(server));
	if (wm_compose(server) == GUI_FLUSH_ERROR)
		stop_requested = 1;
	return 1;
}

static int wm_request_maximize(wm_server *server, wm_window *window, int maximized)
{
	int32_t x;
	int32_t y;
	uint32_t width;
	uint32_t height;
	int result;

	if (window == NULL || window->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
		return -EINVAL;
	if (window->fullscreen || window->mode_pending || window->transition_pending ||
	    window->minimized)
		return -EBUSY;
	if (window->maximized == maximized)
		return 0;
	if (maximized) {
		if (wm_maximize_geometry(server, &x, &y, &width, &height) < 0)
			return -EAGAIN;
		if (!window->maximized) {
			window->normal_x = window->x;
			window->normal_y = window->y;
			window->normal_width = window->width;
			window->normal_height = window->height;
		}
	} else {
		if (!window->maximized)
			return 0;
		x = window->normal_x;
		y = window->normal_y;
		width = window->normal_width;
		height = window->normal_height;
	}
	window->state_pending = 1;
	window->pending_state = maximized ? GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED :
		GUI_PROTOCOL_WINDOW_STATE_NORMAL;
	result = wm_resize_window(server, window, x, y, width, height);
	if (result <= 0) {
		window->state_pending = 0;
		if (result < 0)
			return result;
		window->maximized = maximized;
		if (client_enqueue_window_state(window, maximized ?
		    GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED :
		    GUI_PROTOCOL_WINDOW_STATE_NORMAL) < 0)
			window->client->hello = -1;
		wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, window);
		if (wm_compose(server) == GUI_FLUSH_ERROR)
			stop_requested = 1;
	}
	return 1;
}

static int wm_toggle_fullscreen(wm_server *server, wm_window *window)
{
	size_t bytes_needed;
	int entering;

	if (window == NULL || window->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
		return 0;
	if (window->mode_pending || window->transition_pending ||
	    (server->fullscreen_window != NULL && server->fullscreen_window != window))
		return -EBUSY;
	if (window->configure_serial >= UINT64_MAX - 1)
		return -EOVERFLOW;
	entering = !window->fullscreen;
	if (surface_bytes(entering ? server->state.width : window->normal_width,
	    entering ? server->state.height : window->normal_height, &bytes_needed) < 0)
		return -EINVAL;
	if (client_reserve_surface(window, bytes_needed) < 0)
		return -ENOMEM;
	window->previous_x = window->x;
	window->previous_y = window->y;
	window->previous_width = window->width;
	window->previous_height = window->height;
	window->previous_fullscreen = window->fullscreen;
	if (entering) {
		window->normal_x = window->x;
		window->normal_y = window->y;
		window->normal_width = window->width;
		window->normal_height = window->height;
	}
	window->x = entering ? 0 : window->normal_x;
	window->y = entering ? 0 : window->normal_y;
	window->width = entering ? server->state.width : window->normal_width;
	window->height = entering ? server->state.height : window->normal_height;
	window->fullscreen = entering;
	window->mode_pending = 1;
	window->transition_pending = 1;
	server->fullscreen_window = window;
	wm_cancel_capture(server);
	wm_reset_input_owner(server);
	window->configure_serial++;
	if (client_enqueue_configure(window) < 0) {
		client_fail_surface(window, GUI_PROTOCOL_WINDOW_RESIZE, -EIO);
		window->client->hello = -1;
		return -EIO;
	}
	wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, window);
	return 1;
}

static int wm_move_window(wm_server *server, wm_window *client, int32_t x, int32_t y)
{
	int32_t previous_x = client->x;
	int32_t previous_y = client->y;

	if (client->fullscreen || client->mode_pending)
		return -EBUSY;
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
	WM_HIT_MINIMIZE,
	WM_HIT_MAXIMIZE,
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

static wm_hit_region wm_hit_region_for_client(const wm_window *client, uint32_t x,
					       uint32_t y)
{
	wm_window presented;

	client = wm_presented_window(client, &presented);
	int64_t left = (int64_t)client->x - WM_RESIZE_GRIP;
	int64_t top = (int64_t)client->y - WM_TITLE_HEIGHT - WM_RESIZE_GRIP;
	int64_t right = (int64_t)client->x + client->width + WM_RESIZE_GRIP;
	int64_t bottom = (int64_t)client->y + client->height + WM_RESIZE_GRIP;
	int64_t title_top = (int64_t)client->y - WM_TITLE_HEIGHT;
	int left_band = (int64_t)x < (int64_t)client->x;
	int right_band = (int64_t)x >= (int64_t)client->x + client->width;
	int top_band = (int64_t)y < title_top;
	int bottom_band = (int64_t)y >= (int64_t)client->y + client->height;

	if ((int64_t)x < left || (int64_t)x > right || (int64_t)y < top ||
	    (int64_t)y > bottom)
		return WM_HIT_NONE;
	if (left_band && top_band)
		return WM_HIT_RESIZE_TOP_LEFT;
	if (right_band && top_band)
		return WM_HIT_RESIZE_TOP_RIGHT;
	if (left_band && bottom_band)
		return WM_HIT_RESIZE_BOTTOM_LEFT;
	if (right_band && bottom_band)
		return WM_HIT_RESIZE_BOTTOM_RIGHT;
	if (left_band)
		return WM_HIT_RESIZE_LEFT;
	if (right_band)
		return WM_HIT_RESIZE_RIGHT;
	if (top_band)
		return WM_HIT_RESIZE_TOP;
	if (bottom_band)
		return WM_HIT_RESIZE_BOTTOM;
	if ((int64_t)y < client->y) {
		if ((int64_t)x >= (int64_t)client->x + client->width -
		    3 * WM_TITLE_BUTTON_WIDTH)
			return (int64_t)x >= (int64_t)client->x + client->width -
				2 * WM_TITLE_BUTTON_WIDTH ?
				((int64_t)x >= (int64_t)client->x + client->width -
					WM_TITLE_BUTTON_WIDTH ? WM_HIT_CLOSE :
					WM_HIT_MAXIMIZE) :
				WM_HIT_MINIMIZE;
		return WM_HIT_TITLE;
	}
	return WM_HIT_CONTENT;
}

static wm_window *wm_hit_test(wm_server *server, uint32_t x, uint32_t y,
				      wm_hit_region *region)
{
	wm_window *fullscreen = wm_presented_fullscreen(server);
	wm_window *hit = NULL;
	wm_hit_region hit_region = WM_HIT_NONE;

	if (fullscreen != NULL) {
		if (region != NULL)
			*region = WM_HIT_CONTENT;
		return fullscreen;
	}
	for (wm_window *client = server->windows; client != NULL; client = client->next) {
		wm_hit_region candidate_region;

		if (client->client->fd < 0 || client->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
		    client->window_id == 0 || client->minimized ||
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

static wm_title_button wm_title_button_for_region(wm_hit_region region)
{
	switch (region) {
	case WM_HIT_MINIMIZE: return WM_TITLE_BUTTON_MINIMIZE;
	case WM_HIT_MAXIMIZE: return WM_TITLE_BUTTON_MAXIMIZE;
	case WM_HIT_CLOSE: return WM_TITLE_BUTTON_CLOSE;
	default: return WM_TITLE_BUTTON_NONE;
	}
}

static int wm_update_title_buttons(wm_server *server, wm_window *hit,
				   wm_hit_region region, int left_pressed)
{
	wm_title_button hover = wm_title_button_for_region(region);
	wm_title_button pressed = left_pressed ? hover : WM_TITLE_BUTTON_NONE;
	int changed = 0;

	for (wm_window *window = server->windows; window != NULL; window = window->next) {
		wm_title_button next_hover = window == hit ? hover : WM_TITLE_BUTTON_NONE;
		wm_title_button next_pressed = window == hit ? pressed : WM_TITLE_BUTTON_NONE;

		if (window->hover_button != next_hover) {
			window->hover_button = next_hover;
			changed = 1;
		}
		if (window->pressed_button != next_pressed) {
			window->pressed_button = next_pressed;
			changed = 1;
		}
	}
	return changed;
}

static void wm_release_capture(wm_server *server)
{
	server->capture_client = NULL;
	server->capture_operation = WM_CAPTURE_NONE;
	server->title_click_released = 1;
}

static void wm_cancel_capture(wm_server *server)
{
	wm_release_capture(server);
	server->title_click_window_id = 0;
}

static void wm_begin_capture(wm_server *server, wm_window *client,
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

static wm_window *wm_shell_client(wm_server *server)
{
	for (wm_window *client = server->windows; client != NULL; client = client->next) {

		if (client->client->fd >= 0 && client->client->hello > 0 &&
		    client->client->role == GUI_PROTOCOL_ENDPOINT_SHELL && client->window_id != 0)
			return client;
	}
	return NULL;
}

static int client_enqueue_input_reset(wm_window *client)
{
	return window_enqueue(client, GUI_PROTOCOL_INPUT_RESET, NULL, 0);
}

static void wm_reset_input_owner(wm_server *server)
{
	wm_window *client = server->input_owner;

	if (client != NULL && client_enqueue_input_reset(client) < 0)
		client->client->hello = -1;
}

static void wm_set_input_owner(wm_server *server, wm_window *client)
{
	wm_window *previous = server->input_owner;
	wm_window *fullscreen = server->fullscreen_window;

	if (previous == client)
		return;
	server->title_click_window_id = 0;
	if (fullscreen != NULL && client != NULL && client != fullscreen) {
		if (fullscreen->mode_pending || wm_toggle_fullscreen(server, fullscreen) > 0)
			server->deferred_focus_id = client->window_id;
		return;
	}
	/*
	 * Reset both sides of a transfer.  The old client drops stuck state before
	 * losing ownership, while the new client cannot inherit a key or button
	 * that was physically pressed before it became the recipient.  Future
	 * pointer-grab transitions use this same ordered boundary.
	 */
	if (previous != NULL) {
		if (previous->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY) {
			previous->focused = 0;
			if (client_enqueue_focus(previous, 0) < 0)
				previous->client->hello = -1;
			wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, previous);
		}
		if (client_enqueue_input_reset(previous) < 0)
			previous->client->hello = -1;
	}
	server->input_owner = client;
	if (client != NULL) {
		if (client->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY) {
			client->stack_order = ++server->next_stack_order;
			client->focused = 1;
			if (client_enqueue_focus(client, 1) < 0)
				client->client->hello = -1;
			wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
		}
		if (client_enqueue_input_reset(client) < 0)
			client->client->hello = -1;
	}
}

static wm_window *wm_next_focus(wm_server *server)
{
	wm_window *next = NULL;

	for (wm_window *client = server->windows; client != NULL; client = client->next) {

		if (client->client->fd >= 0 && client->client->hello > 0 &&
		    client->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
		    client->window_id != 0 && !client->minimized &&
		    (next == NULL || client->stack_order > next->stack_order))
			next = client;
	}
	return next;
}

static void wm_complete_mode(wm_window *window)
{
	wm_server *server = window->server;
	uint32_t deferred = server->deferred_focus_id;

	wm_reset_input_owner(server);
	if (window->fullscreen) {
		if (deferred != 0 && wm_toggle_fullscreen(server, window) < 0)
			server->deferred_focus_id = 0;
		return;
	}
	server->fullscreen_window = NULL;
	server->deferred_focus_id = 0;
	if (deferred != 0) {
		wm_window *target = wm_find_window(server, deferred);

		if (target != NULL && target->client->hello > 0)
			wm_set_input_owner(server, target);
		if (wm_compose(server) == GUI_FLUSH_ERROR)
			stop_requested = 1;
	}
}

static void wm_disconnect_client(wm_server *server, wm_client *client)
{
	int focused = server->input_owner != NULL && server->input_owner->client == client;

	client->hello = -1;
	if (server->capture_client != NULL && server->capture_client->client == client)
		wm_cancel_capture(server);
	if (client->role == GUI_PROTOCOL_ENDPOINT_SHELL) {
		wm_window *desktop = client->windows;

		wm_discard_shell_list(server);
		server->shell_pid = 0;
		if (server->shell_replacement_started || !server->shell_ready ||
		    desktop == NULL || desktop->committed == NULL || server->desktop_snapshot != NULL) {
			if (focused)
				server->input_owner = NULL;
			client_destroy(client);
			stop_requested = 1;
			return;
		}
		server->desktop_snapshot = desktop->committed;
		server->desktop_snapshot_bytes = desktop->committed_bytes;
		client->surface_bytes -= desktop->committed_bytes;
		desktop->committed = NULL;
		desktop->committed_bytes = 0;
		if (focused)
			wm_set_input_owner(server, wm_next_focus(server));
		client_destroy(client);
		server->shell_ready = 0;
		server->shell_replacement_started = 1;
		if (wm_start_shell(server) < 0 || wm_compose(server) == GUI_FLUSH_ERROR)
			stop_requested = 1;
		return;
	}
	if (focused)
		wm_set_input_owner(server, NULL);
	for (wm_window *window = client->windows; window != NULL; window = window->owner_next)
		wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_REMOVE, window);
	client_destroy(client);
	if (focused)
		wm_set_input_owner(server, wm_next_focus(server));
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
	case 0x148: return GUI_KEYSYM_UP;
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

static wm_window *wm_alt_tab_target(wm_server *server)
{
	wm_window *current = server->input_owner;
	wm_window *next = NULL;
	wm_window *wrap = NULL;

	for (wm_window *candidate = server->windows; candidate != NULL; candidate = candidate->next) {

		if (candidate->client->fd < 0 || candidate->client->hello <= 0 ||
		    candidate->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
		    candidate->window_id == 0)
			continue;
		if (wrap == NULL || candidate->stack_order > wrap->stack_order)
			wrap = candidate;
		if (current != NULL && current->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
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

static int wm_update_cursor(wm_server *server)
{
	wm_cursor_shape shape = WM_CURSOR_ARROW;
	wm_capture_operation operation = WM_CAPTURE_NONE;

	if (server->state.graphics && server->state.cursor_loaded) {
		if (server->capture_client != NULL) {
			operation = server->capture_operation;
		} else {
			wm_hit_region region;
			wm_window *hit = wm_hit_test(server, server->state.cursor_x,
				server->state.cursor_y, &region);

			if (hit != NULL && !hit->mode_pending && !hit->fullscreen) {
				operation = wm_capture_operation_for_region(region);
				if (operation == WM_CAPTURE_MOVE)
					operation = WM_CAPTURE_NONE;
			}
		}
	}
	switch (operation) {
	case WM_CAPTURE_MOVE:
		shape = WM_CURSOR_MOVE;
		break;
	case WM_CAPTURE_RESIZE_LEFT:
	case WM_CAPTURE_RESIZE_RIGHT:
		shape = WM_CURSOR_RESIZE_HORIZONTAL;
		break;
	case WM_CAPTURE_RESIZE_TOP:
	case WM_CAPTURE_RESIZE_BOTTOM:
		shape = WM_CURSOR_RESIZE_VERTICAL;
		break;
	case WM_CAPTURE_RESIZE_TOP_LEFT:
	case WM_CAPTURE_RESIZE_BOTTOM_RIGHT:
		shape = WM_CURSOR_RESIZE_NW_SE;
		break;
	case WM_CAPTURE_RESIZE_TOP_RIGHT:
	case WM_CAPTURE_RESIZE_BOTTOM_LEFT:
		shape = WM_CURSOR_RESIZE_NE_SW;
		break;
	case WM_CAPTURE_NONE:
		break;
	}
	if (server->state.cursor_shape == shape)
		return 0;
	server->state.cursor_shape = shape;
	return 1;
}

static int wm_update_capture(wm_server *server)
{
	wm_window *client = server->capture_client;
	int64_t dx = (int64_t)server->state.cursor_x - server->capture_x;
	int64_t dy = (int64_t)server->state.cursor_y - server->capture_y;
	int32_t x = server->capture_window_x;
	int32_t y = server->capture_window_y;
	uint32_t width = server->capture_width;
	uint32_t height = server->capture_height;
	int resize = 0;

	if (client == NULL || client->client->fd < 0 || client->window_id == 0)
		return 0;
	if (server->capture_operation == WM_CAPTURE_MOVE)
		return wm_move_window(server, client, (int32_t)(x + dx), (int32_t)(y + dy));
	if (server->capture_operation == WM_CAPTURE_RESIZE_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_BOTTOM_LEFT) {
		int64_t right = (int64_t)x + width;
		int64_t maximum_x = right - wm_minimum_width(server);
		int64_t proposed_x = (int64_t)x + dx;

		if (proposed_x < WM_RESIZE_GRIP)
			proposed_x = WM_RESIZE_GRIP;
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
			server->state.width > (uint32_t)x + WM_RESIZE_GRIP ?
			server->state.width - x - WM_RESIZE_GRIP : wm_minimum_width(server));
		resize = 1;
	}
	if (server->capture_operation == WM_CAPTURE_RESIZE_TOP ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_LEFT ||
	    server->capture_operation == WM_CAPTURE_RESIZE_TOP_RIGHT) {
		int64_t bottom = (int64_t)y + height;
		int64_t maximum_y = bottom - wm_minimum_height(server);
		int64_t proposed_y = (int64_t)y + dy;

		if (proposed_y < WM_TITLE_HEIGHT + WM_RESIZE_GRIP)
			proposed_y = WM_TITLE_HEIGHT + WM_RESIZE_GRIP;
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
			server->state.height > (uint32_t)y + WM_RESIZE_GRIP ?
			server->state.height - y - WM_RESIZE_GRIP : wm_minimum_height(server));
		resize = 1;
	}
	if (!resize || (x == client->x && y == client->y && width == client->width &&
			height == client->height))
		return 0;
	resize = wm_resize_window(server, client, x, y, width, height);
	if (resize < 0)
		window_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, resize);
	return 0;
}

static void wm_emit_input(void *context, const gui_input_event *event)
{
	wm_server *server = context;
	wm_state *state = &server->state;
	wm_window *client;
	uint8_t message[sizeof(gui_protocol_wire_input_event)] = { 0 };
	uint32_t keysym = 0;
	int32_t x;
	int32_t y;
	int compose = 0;

	if (event->type == GUI_INPUT_EVENT_KEY_RESET) {
		wm_reset_key_state(state);
		wm_cancel_capture(server);
		wm_update_title_buttons(server, NULL, WM_HIT_NONE, 0);
		wm_reset_input_owner(server);
		wm_present_input(server, 0, 0);
		return;
	}
	if (event->type == GUI_INPUT_EVENT_MOUSE_RESET) {
		state->mouse_buttons = 0;
		wm_cancel_capture(server);
		wm_update_title_buttons(server, NULL, WM_HIT_NONE, 0);
		wm_reset_input_owner(server);
		wm_present_input(server, 0, 0);
		return;
	}
	if (!server->state.graphics)
		return;
	if (event->type == GUI_INPUT_EVENT_KEY) {
		wm_update_modifiers(state, event->key.scancode, event->key.pressed);
		if (!event->key.pressed)
			return;
		server->title_click_window_id = 0;
		keysym = wm_keysym(event->key.scancode, state->modifiers);
		if (keysym == '\t' && (state->modifiers & GUI_MOD_ALT)) {
			wm_window *target = wm_alt_tab_target(server);

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
		if (keysym == 'f' && (state->modifiers & GUI_MOD_SUPER)) {
			int result = wm_toggle_fullscreen(server, server->input_owner);

			if (result < 0)
				window_reject(server->input_owner, GUI_PROTOCOL_WINDOW_RESIZE, result);
			return;
		}
		if (keysym == 0)
			return;
	}
	if (event->type == GUI_INPUT_EVENT_MOUSE) {
		uint8_t previous_buttons = state->mouse_buttons;
		int motion_only = previous_buttons == event->mouse.buttons;
		uint32_t cursor_x = state->cursor_x;
		uint32_t cursor_y = state->cursor_y;

		gui_input_apply_mouse_motion(event, state->width, state->height,
			&cursor_x, &cursor_y);
		if (state->cursor_x != cursor_x || state->cursor_y != cursor_y) {
			state->cursor_x = cursor_x;
			state->cursor_y = cursor_y;
			compose = 1;
		}
		state->mouse_buttons = event->mouse.buttons;
		if (server->title_click_window_id != 0) {
			int64_t dx = (int64_t)state->cursor_x - server->title_click_x;
			int64_t dy = (int64_t)state->cursor_y - server->title_click_y;

			if (wm_now_ms() - server->title_click_ms > WM_TITLE_DOUBLE_CLICK_MS ||
			    dx < -WM_TITLE_CLICK_SLOP || dx > WM_TITLE_CLICK_SLOP ||
			    dy < -WM_TITLE_CLICK_SLOP || dy > WM_TITLE_CLICK_SLOP ||
			    ((previous_buttons | state->mouse_buttons) & ~1U))
				server->title_click_window_id = 0;
		}
		if (server->capture_client != NULL) {
			compose |= wm_update_title_buttons(server, NULL, WM_HIT_NONE, 0);
			if (state->mouse_buttons & 1)
				compose |= wm_update_capture(server);
			else
				wm_release_capture(server);
			wm_present_input(server, compose, motion_only);
			return;
		}
		{
			wm_hit_region region;
			wm_window *hit = wm_hit_test(server, state->cursor_x, state->cursor_y, &region);

			compose |= wm_update_title_buttons(server, hit, region,
				state->mouse_buttons & 1);
			if (!(previous_buttons & 1) && (state->mouse_buttons & 1)) {
				wm_capture_operation operation =
					wm_capture_operation_for_region(region);
				int double_click = region == WM_HIT_TITLE && hit != NULL &&
					server->title_click_window_id == hit->window_id &&
					server->title_click_released;

				server->title_click_window_id = 0;

				if (hit == NULL)
					hit = wm_shell_client(server);
				if (hit != NULL) {
					wm_set_input_owner(server, hit);
					compose = 1;
				}
				if (hit != NULL &&
				    hit->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY &&
				    !hit->mode_pending) {
					if (double_click) {
						int result = wm_request_maximize(server, hit, !hit->maximized);

						if (result < 0)
							window_reject(hit,
								GUI_PROTOCOL_WINDOW_STATE_REQUEST, result);
						wm_present_input(server, compose, 0);
						return;
					}
					if (operation != WM_CAPTURE_NONE) {
						wm_begin_capture(server, hit, operation);
						if (operation == WM_CAPTURE_MOVE && state->mouse_buttons == 1 &&
						    !hit->transition_pending) {
							server->title_click_window_id = hit->window_id;
							server->title_click_x = state->cursor_x;
							server->title_click_y = state->cursor_y;
							server->title_click_ms = wm_now_ms();
							server->title_click_released = 0;
						}
						wm_present_input(server, compose, 0);
						return;
					}
					switch (region) {
					case WM_HIT_MINIMIZE: {
						int result = wm_request_minimize(server, hit);

						if (result < 0)
							window_reject(hit,
								GUI_PROTOCOL_WINDOW_STATE_REQUEST, result);
						break;
					}
					case WM_HIT_MAXIMIZE: {
						int result = wm_request_maximize(server, hit,
							!hit->maximized);

						if (result < 0)
							window_reject(hit,
								GUI_PROTOCOL_WINDOW_STATE_REQUEST, result);
						break;
					}
					case WM_HIT_CLOSE:
						client_enqueue_close_request(hit);
						break;
					default:
						break;
					}
					if (region == WM_HIT_MINIMIZE ||
					    region == WM_HIT_MAXIMIZE ||
					    region == WM_HIT_CLOSE) {
						wm_present_input(server, compose, 0);
						return;
					}
				}
			}
		}
		wm_present_input(server, compose, motion_only);
	}
	client = server->input_owner;
	if (client == NULL || client->mode_pending)
		return;
	put_u32(message, client->window_id);
	put_u32(message + 4, event->type);
	if (event->type == GUI_INPUT_EVENT_KEY)
		put_u32(message + 8, keysym);
	put_u32(message + 12, state->modifiers);
	x = state->cursor_x;
	y = state->cursor_y;
	if (client->client->role == GUI_PROTOCOL_ENDPOINT_ORDINARY) {
		x -= client->x;
		y -= client->y;
	}
	put_u32(message + 16, (uint32_t)x);
	put_u32(message + 20, (uint32_t)y);
	if (event->type == GUI_INPUT_EVENT_MOUSE)
		message[24] = event->mouse.buttons;
	put_u32(message + 28, event->ticks);
	if (window_enqueue(client, GUI_PROTOCOL_INPUT_EVENT, message,
			   sizeof(message)) < 0)
		client->client->hello = -1;
}

static int wm_read_input(wm_server *server, int descriptor, int mouse)
{
	union {
		struct mario_gui_key_event keys[WM_INPUT_BATCH_RECORDS];
		struct mario_psaux_event mice[WM_INPUT_BATCH_RECORDS];
	} records;
	size_t record_bytes = mouse ? sizeof(records.mice[0]) : sizeof(records.keys[0]);

	for (unsigned int batch = 0; batch < WM_INPUT_READ_QUANTUM; batch++) {
		ssize_t count = read(descriptor, &records, record_bytes * WM_INPUT_BATCH_RECORDS);

		if (count > 0) {
			if (mouse)
				gui_input_normalize_mouse_records(records.mice, count / record_bytes,
					wm_emit_input, server);
			else
				gui_input_normalize_key_records(records.keys, count / record_bytes,
					wm_emit_input, server);
		} else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return 0;
		} else if (count == 0 || errno != EINTR) {
			return -1;
		}
		if (wm_present_motion(server) < 0)
			return -1;
	}
	return 0;
}

static void window_message(wm_window *client, const gui_protocol_header *header,
			   const uint8_t *bytes, size_t payload_bytes)
{
	if (header->type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN) {
		(void)client_stage_patch(client, bytes);
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_CANCEL) {
		client_cancel_surface(client, bytes);
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_SET_TITLE) {
		uint16_t title_bytes;

		if (client->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
			window_reject(client, GUI_PROTOCOL_WINDOW_SET_TITLE, -EPERM);
		else if (payload_bytes < sizeof(gui_protocol_wire_window_set_title) ||
			 get_u32(bytes) != client->window_id || client->window_id == 0 ||
			 (title_bytes = get_u16(bytes + 4)) > GUI_PROTOCOL_MAX_TITLE_BYTES ||
			 payload_bytes != sizeof(gui_protocol_wire_window_set_title) + title_bytes ||
			 memchr(bytes + sizeof(gui_protocol_wire_window_set_title), '\0', title_bytes) != NULL)
			window_reject(client, GUI_PROTOCOL_WINDOW_SET_TITLE, -EINVAL);
		else {
			client->title_bytes = title_bytes;
			memcpy(client->title, bytes + sizeof(gui_protocol_wire_window_set_title),
			       title_bytes);
			wm_notify_shell_window(client->server, GUI_PROTOCOL_WINDOW_LIST_CHANGE, client);
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_BEGIN &&
	    payload_bytes == sizeof(gui_protocol_wire_surface_begin)) {
		if (client_stage_surface(client, bytes) < 0)
			client->client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_DATA &&
	    payload_bytes >= sizeof(gui_protocol_wire_surface_data)) {
		if (client_surface_data(client, bytes, payload_bytes) < 0)
			client->client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_SURFACE_COMMIT &&
	    payload_bytes == sizeof(gui_protocol_wire_surface_commit)) {
		if (client_commit_surface(client, bytes) < 0)
			client->client->hello = -1;
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_MOVE &&
	    payload_bytes == sizeof(gui_protocol_wire_window_move)) {
		if (client->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
			window_reject(client, GUI_PROTOCOL_WINDOW_MOVE, -EPERM);
		else if (get_u32(bytes) != client->window_id || client->window_id == 0)
			window_reject(client, GUI_PROTOCOL_WINDOW_MOVE, -EINVAL);
		else {
			int result = wm_move_window(client->server, client, (int32_t)get_u32(bytes + 4),
				(int32_t)get_u32(bytes + 8));

			if (result < 0)
				window_reject(client, GUI_PROTOCOL_WINDOW_MOVE, result);
			else if (wm_compose(client->server) == GUI_FLUSH_ERROR)
				client->client->hello = -1;
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_RESIZE &&
	    payload_bytes == sizeof(gui_protocol_wire_window_resize)) {
		int result;

		if (client->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY)
			window_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, -EPERM);
		else if (get_u32(bytes) != client->window_id || client->window_id == 0)
			window_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, -EINVAL);
		else {
			result = wm_resize_window(client->server, client, client->x, client->y,
				get_u32(bytes + 4), get_u32(bytes + 8));
			if (result < 0)
				window_reject(client, GUI_PROTOCOL_WINDOW_RESIZE, result);
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_DESTROY && payload_bytes == 4) {
		wm_server *server = client->server;
		wm_client *owner = client->client;
		int ordinary = owner->role == GUI_PROTOCOL_ENDPOINT_ORDINARY;

		if (server->capture_client == client)
			wm_cancel_capture(server);
		if (server->input_owner == client)
			wm_set_input_owner(server, NULL);
		if (ordinary)
			wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_REMOVE, client);
		window_destroy(client);
		if (ordinary && server->input_owner == NULL)
			wm_set_input_owner(server, wm_next_focus(server));
		if (wm_compose(server) == GUI_FLUSH_ERROR)
			owner->hello = -1;
		return;
	}
	client->client->hello = -1;
}

static void client_create_window(wm_client *client, const uint8_t *bytes,
				size_t payload_bytes, int desktop)
{
	wm_server *server = client->server;
	wm_window *window;
	uint8_t reply[sizeof(gui_protocol_wire_window_create_reply)];
	uint32_t width = desktop ? server->state.width : get_u32(bytes + 4);
	uint32_t height = desktop ? server->state.height : get_u32(bytes + 8);
	uint16_t title_bytes = desktop ? 0 : get_u16(bytes + 12);
	gui_protocol_type type = desktop ? GUI_PROTOCOL_DESKTOP_CREATE : GUI_PROTOCOL_WINDOW_CREATE;
	size_t window_bytes;

	if (client->role != (desktop ? GUI_PROTOCOL_ENDPOINT_SHELL : GUI_PROTOCOL_ENDPOINT_ORDINARY)) {
		client_reject(client, type, -EPERM);
		return;
	}
	if (desktop && client->windows != NULL) {
		client_reject(client, type, -EBUSY);
		return;
	}
	if ((!desktop && (payload_bytes != sizeof(gui_protocol_wire_window_create) + title_bytes ||
	     title_bytes > GUI_PROTOCOL_MAX_TITLE_BYTES ||
	     memchr(bytes + sizeof(gui_protocol_wire_window_create), 0, title_bytes) != NULL)) ||
	    width == 0 || height == 0 || width > server->state.width || height > server->state.height ||
	    surface_bytes(width, height, &window_bytes) < 0) {
		client_reject(client, type, -EINVAL);
		return;
	}
	if (server->next_window_id == UINT32_MAX) {
		client_reject(client, type, -EOVERFLOW);
		return;
	}
	window = wm_record_create(server, client, sizeof(*window));
	if (window == NULL) {
		client_reject(client, type, -ENOMEM);
		return;
	}
	window->server = server;
	window->client = client;
	if (client_reserve_surface(window, window_bytes) < 0) {
		window_destroy(window);
		client_reject(client, type, -ENOMEM);
		return;
	}
	window->window_id = wm_allocate_window_id(server);
	window->width = width;
	window->height = height;
	window->configure_serial = 1;
	window->title_bytes = title_bytes;
	if (!desktop) {
		memcpy(window->title, bytes + sizeof(gui_protocol_wire_window_create), title_bytes);
		wm_place_window(server, window);
	}
	window->next = server->windows;
	server->windows = window;
	window->owner_next = client->windows;
	client->windows = window;
	put_u32(reply, get_u32(bytes));
	put_u32(reply + 4, window->window_id);
	put_u64(reply + 8, window->configure_serial);
	put_u32(reply + 16, width);
	put_u32(reply + 20, height);
	if (client_enqueue(client, desktop ? GUI_PROTOCOL_DESKTOP_CREATE_REPLY :
	    GUI_PROTOCOL_WINDOW_CREATE_REPLY, reply, sizeof(reply)) < 0) {
		client->hello = -1;
	} else if (desktop) {
		wm_send_shell_snapshot(server, client);
	} else {
		wm_notify_shell_window(server, GUI_PROTOCOL_WINDOW_LIST_ADD, window);
		if (server->fullscreen_window == NULL)
			wm_set_input_owner(server, window);
		else {
			if (client_enqueue_focus(window, 0) < 0 || client_enqueue_input_reset(window) < 0)
				client->hello = -1;
			server->fullscreen_window->stack_order = ++server->next_stack_order;
		}
	}
}

static void client_message(void *context, const gui_protocol_header *header,
			   const void *payload, size_t payload_bytes)
{
	wm_client *client = context;
	const uint8_t *bytes = payload;
	wm_window *window;
	uint8_t reply[sizeof(gui_protocol_wire_hello)];
	size_t window_payload_bytes;
	int variable_payload = 0;

	if (!client->hello) {
		if (header->type != GUI_PROTOCOL_HELLO || payload_bytes != sizeof(reply) ||
		    get_u16(bytes) != GUI_PROTOCOL_VERSION_MAJOR || get_u16(bytes + 2) > GUI_PROTOCOL_VERSION_MINOR) {
			client->hello = -1;
			return;
		}
		put_u16(reply, GUI_PROTOCOL_VERSION_MAJOR);
		client->protocol_minor = get_u16(bytes + 2);
		put_u16(reply + 2, client->protocol_minor);
		client->hello = client_enqueue(client, GUI_PROTOCOL_HELLO_REPLY, reply, sizeof(reply)) < 0 ? -1 : 1;
		return;
	}
	if (header->type == GUI_PROTOCOL_CAPABILITIES) {
		uint8_t capabilities[sizeof(gui_protocol_wire_capabilities)];

		if (payload_bytes != sizeof(capabilities)) {
			client_reject(client, header->type, -EINVAL);
		} else if (client->protocol_minor < 1) {
			client_reject(client, header->type, -EPROTONOSUPPORT);
		} else if (client->windows != NULL) {
			client_reject(client, header->type, -EBUSY);
		} else {
			client->capabilities = get_u32(bytes) & GUI_PROTOCOL_CAP_SURFACE_PATCH;
			put_u32(capabilities, client->capabilities);
			if (client_enqueue(client, GUI_PROTOCOL_CAPABILITIES_REPLY, capabilities, sizeof(capabilities)) < 0)
				client->hello = -1;
		}
		return;
	}
	if (header->type == GUI_PROTOCOL_LAUNCH_REQUEST) {
		client_handle_launch_request(client, bytes, payload_bytes);
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CREATE) {
		if (payload_bytes < sizeof(gui_protocol_wire_window_create))
			client_reject(client, header->type, -EINVAL);
		else
			client_create_window(client, bytes, payload_bytes, 0);
		return;
	}
	if (header->type == GUI_PROTOCOL_DESKTOP_CREATE &&
	    payload_bytes == sizeof(gui_protocol_wire_desktop_create)) {
		client_create_window(client, bytes, payload_bytes, 1);
		return;
	}
	if (header->type == GUI_PROTOCOL_SHELL_WORK_AREA) {
		if (client->role != GUI_PROTOCOL_ENDPOINT_SHELL) {
			client_reject(client, header->type, -EPERM);
			return;
		}
		if (payload_bytes != sizeof(gui_protocol_wire_shell_work_area)) {
			client_reject(client, header->type, -EINVAL);
			return;
		}
		client->server->work_x = (int32_t)get_u32(bytes);
		client->server->work_y = (int32_t)get_u32(bytes + 4);
		client->server->work_width = get_u32(bytes + 8);
		client->server->work_height = get_u32(bytes + 12);
		client->server->work_area_valid = 1;
		return;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_STATE_REQUEST) {
		uint32_t state;
		int result;

		if (payload_bytes != sizeof(gui_protocol_wire_window_state_request)) {
			client_reject(client, header->type, -EINVAL);
			return;
		}
		window = wm_find_window(client->server, get_u32(bytes));
		state = get_u32(bytes + 4);
		if (window == NULL || window->client != client ||
		    window->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY) {
			client_reject(client, header->type, -ENOENT);
			return;
		}
		switch (state) {
		case GUI_PROTOCOL_WINDOW_STATE_MINIMIZED:
			result = wm_request_minimize(client->server, window);
			break;
		case GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED:
			result = wm_request_maximize(client->server, window, 1);
			break;
		case GUI_PROTOCOL_WINDOW_STATE_NORMAL:
			if (window->minimized)
				result = wm_request_minimize(client->server, window);
			else if (window->maximized)
				result = wm_request_maximize(client->server, window, 0);
			else
				result = 0;
			break;
		default:
			client_reject(client, header->type, -EINVAL);
			return;
		}
		if (result < 0)
			window_reject(window, header->type, result);
		return;
	}
	if (header->type == GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW && payload_bytes == 4) {
		int result;

		if (client->role != GUI_PROTOCOL_ENDPOINT_SHELL) {
			client_reject(client, header->type, -EPERM);
			return;
		}
		window = wm_find_window(client->server, get_u32(bytes));
		if (window == NULL || window->client->role != GUI_PROTOCOL_ENDPOINT_ORDINARY ||
		    window->client->hello <= 0) {
			client_reject(client, header->type, -ENOENT);
			return;
		}
		if (window->minimized) {
			result = wm_request_minimize(client->server, window);
			if (result < 0) {
				client_reject(client, header->type, result);
				return;
			}
		}
		wm_set_input_owner(client->server, window);
		if (wm_compose(client->server) == GUI_FLUSH_ERROR)
			client->hello = -1;
		return;
	}
	switch (header->type) {
	case GUI_PROTOCOL_WINDOW_SET_TITLE:
		window_payload_bytes = sizeof(gui_protocol_wire_window_set_title);
		variable_payload = 1;
		break;
	case GUI_PROTOCOL_SURFACE_BEGIN:
		window_payload_bytes = sizeof(gui_protocol_wire_surface_begin);
		break;
	case GUI_PROTOCOL_SURFACE_PATCH_BEGIN:
		window_payload_bytes = sizeof(gui_protocol_wire_surface_patch_begin);
		break;
	case GUI_PROTOCOL_SURFACE_CANCEL:
		window_payload_bytes = sizeof(gui_protocol_wire_surface_cancel);
		break;
	case GUI_PROTOCOL_SURFACE_DATA:
		window_payload_bytes = sizeof(gui_protocol_wire_surface_data);
		variable_payload = 1;
		break;
	case GUI_PROTOCOL_SURFACE_COMMIT:
		window_payload_bytes = sizeof(gui_protocol_wire_surface_commit);
		break;
	case GUI_PROTOCOL_WINDOW_MOVE:
		window_payload_bytes = sizeof(gui_protocol_wire_window_move);
		break;
	case GUI_PROTOCOL_WINDOW_RESIZE:
		window_payload_bytes = sizeof(gui_protocol_wire_window_resize);
		break;
	case GUI_PROTOCOL_WINDOW_DESTROY:
		window_payload_bytes = sizeof(gui_protocol_wire_window_id);
		break;
	default:
		client->hello = -1;
		return;
	}
	if (payload_bytes < window_payload_bytes ||
	    (!variable_payload && payload_bytes != window_payload_bytes)) {
		if (header->type == GUI_PROTOCOL_SURFACE_PATCH_BEGIN || header->type == GUI_PROTOCOL_SURFACE_CANCEL) {
			client_reject(client, header->type, -EINVAL);
			return;
		}
		client->hello = -1;
		return;
	}
	window = wm_find_window(client->server, get_u32(bytes));
	if (window == NULL || window->client != client)
		client_reject(client, header->type, -EINVAL);
	else
		window_message(window, header, bytes, payload_bytes);
}

static void wm_cleanup(wm_state *state)
{
	/* Close the listener before releasing the descriptors that own the seat. */
	if (state->listener >= 0)
		close(state->listener);
	if (state->owns_status)
		gui_session_remove_status_files();
	if (state->gui >= 0)
		close(state->gui);
	if (state->mouse >= 0)
		close(state->mouse);
	if (state->framebuffer >= 0)
		close(state->framebuffer);
	gui_surface_destroy(state->composite);
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

static void shell_exec(int fd, int diagnostics)
{
	if (fd != WM_SHELL_FD) {
		if (dup2(fd, WM_SHELL_FD) < 0)
			_exit(127);
		close(fd);
	}
	/* All server descriptors are close-on-exec; fd 3 is the shell's only capability. */
	if (diagnostics)
		execl(WM_SHELL_PATH, WM_SHELL_PATH, "--wm-fd=3", "--diagnostics", NULL);
	else
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
		shell_exec(fds[1], server->diagnostics);
	}
	close(fds[1]);
	if (wm_add_client(server, fds[0], GUI_PROTOCOL_ENDPOINT_SHELL) == NULL) {
		kill(pid, SIGTERM);
		return -1;
	}
	server->shell_pid = pid;
	return 0;
}

static void wm_stop_shell(wm_server *server)
{
	if (server->shell_pid <= 0)
		return;
	/* The private socket alone is not a lifetime guarantee during WM shutdown. */
	kill(server->shell_pid, SIGTERM);
	server->shell_pid = 0;
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

static int wm_start_terminal_command(const uint8_t *path, size_t path_bytes)
{
	char command[GUI_PROTOCOL_MAX_PATH_BYTES * 5 + sizeof("/usr/bin/vim -- ''")];
	char *cursor = command;
	pid_t pid;

	if (path_bytes == 0 || path_bytes > GUI_PROTOCOL_MAX_PATH_BYTES) {
		errno = EINVAL;
		return -1;
	}
	/* terminal passes this command to oksh, so quote the pathname byte by byte. */
	cursor += sprintf(cursor, "/usr/bin/vim -- '");
	for (size_t index = 0; index < path_bytes; index++) {
		if (path[index] == '\'') {
			memcpy(cursor, "'\\''", 4);
			cursor += 4;
		} else {
			*cursor++ = path[index];
		}
	}
	*cursor++ = '\'';
	*cursor = '\0';
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execl(WM_TERMINAL_PATH, WM_TERMINAL_PATH, "--command", command, NULL);
		_exit(127);
	}
	return 0;
}

static int wm_start_terminal_executable(wm_server *server, const uint8_t *path,
					size_t path_bytes, uint32_t origin_window_id)
{
	char command[GUI_PROTOCOL_MAX_PATH_BYTES * 5 + sizeof("exec ''")];
	char *cursor = command;
	wm_launch *launch;
	wm_window *origin;
	pid_t pid;

	if (path_bytes == 0 || path_bytes > GUI_PROTOCOL_MAX_PATH_BYTES ||
	    origin_window_id == 0) {
		errno = EINVAL;
		return -1;
	}
	origin = wm_find_window(server, origin_window_id);
	if (origin == NULL) {
		errno = EINVAL;
		return -1;
	}
	launch = wm_record_create(server, origin->client, sizeof(*launch));
	if (launch == NULL) {
		return -1;
	}
	/* terminal passes this command to oksh, so quote the pathname byte by byte. */
	cursor += sprintf(cursor, "exec '");
	for (size_t index = 0; index < path_bytes; index++) {
		if (path[index] == '\'') {
			memcpy(cursor, "'\\''", 4);
			cursor += 4;
		} else {
			*cursor++ = path[index];
		}
	}
	*cursor++ = '\'';
	*cursor = '\0';
	pid = fork();
	if (pid < 0) {
		int saved_errno = errno;

		wm_record_destroy(server, origin->client, launch, sizeof(*launch));
		errno = saved_errno;
		return -1;
	}
	if (pid == 0) {
		execl(WM_TERMINAL_PATH, WM_TERMINAL_PATH, "--command", command, NULL);
		_exit(127);
	}
	launch->pid = pid;
	launch->origin_window_id = origin_window_id;
	launch->client = origin->client;
	launch->next = server->launches;
	server->launches = launch;
	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction action = { .sa_handler = request_stop };
	struct sigaction ignore_pipe = { .sa_handler = SIG_IGN };
	struct sigaction child_action = { .sa_handler = request_child_reap };
	struct pollfd *pollfds;
	wm_client **poll_clients;
	wm_server server = { .state = { .framebuffer = -1, .gui = -1,
		.mouse = -1, .listener = -1 } };
	int status = 1;

	if (argc > 2 || (argc == 2 && strcmp(argv[1], "--diagnostics") != 0)) {
		fprintf(stderr, "mario-wm: usage: mario-wm [--diagnostics]\n");
		return 1;
	}
	server.diagnostics = argc == 2;
	/* Preserve the previous cascade origin so normal applications stay put. */
	server.next_placement = server.diagnostics ? 0 : WM_DIAGNOSTIC_COUNT;

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
	gui_session_remove_status_files();
	server.state.listener = bind_listener();
	if (server.state.listener < 0)
		goto out;
	printf("mario-wm: listening\n");
	fflush(stdout);
	/* Socket write failures are handled per-client by the output queue. */
	if (sigaction(SIGTERM, &action, NULL) < 0 ||
	    sigaction(SIGINT, &action, NULL) < 0 ||
	    sigaction(SIGPIPE, &ignore_pipe, NULL) < 0 ||
	    sigaction(SIGCHLD, &child_action, NULL) < 0)
		goto out;
	if (wm_start_shell(&server) < 0)
		goto out;
	if (create_started_marker() < 0)
		goto out;
	while (!stop_requested) {
		int ready;
		int poll_timeout = -1;
		size_t poll_count = 4;

		if (child_reap_requested) {
			child_reap_requested = 0;
			wm_reap_children(&server);
		}

		pollfds = server.poll_storage;
		poll_clients = (wm_client **)(pollfds + server.poll_capacity + 4);
		pollfds[0] = (struct pollfd) { .fd = server.state.gui, .events = POLLIN };
		pollfds[1] = (struct pollfd) { .fd = server.state.framebuffer, .events = POLLPRI };
		pollfds[2] = (struct pollfd) { .fd = server.state.listener, .events = POLLIN };
		pollfds[3] = (struct pollfd) { .fd = server.state.mouse, .events = POLLIN };
		for (wm_client *client = server.clients; client != NULL; client = client->next) {
			short events = POLLIN;

			wm_pump_shell_snapshot(&server, client);
			if (client->drain_input || ((client->snapshot_pending ||
			    (client->role == GUI_PROTOCOL_ENDPOINT_SHELL && server.list_pending != 0)) &&
			    !gui_protocol_output_pending(client->output)))
				poll_timeout = 0;
			if (gui_protocol_output_pending(client->output))
				events |= POLLOUT;
			poll_clients[poll_count - 4] = client;
			pollfds[poll_count++] = (struct pollfd) { .fd = client->fd, .events = events };
		}
		{
			int motion_timeout = wm_motion_timeout(&server);

			if (motion_timeout >= 0 && (poll_timeout < 0 || motion_timeout < poll_timeout))
				poll_timeout = motion_timeout;
		}
		ready = poll(pollfds, poll_count, poll_timeout);

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
			server.state.scanout_busy = 0;
			server.state.motion_pending = 0;
			wm_damage_init(&server.state);
			gui_region_mark_full(&server.state.damage);
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
		if ((pollfds[0].revents & POLLIN) && wm_read_input(&server, server.state.gui, 0) < 0)
			goto out;
		if ((pollfds[3].revents & POLLIN) && wm_read_input(&server, server.state.mouse, 1) < 0)
			goto out;
		if (wm_present_motion(&server) < 0)
			goto out;
		/* Mouse-driven moves and resizes queue configure messages without I/O. */
		wm_flush_client_outputs(&server);
		for (size_t poll_index = 4; poll_index < poll_count; poll_index++) {
			wm_client *client = poll_clients[poll_index - 4];
			uint8_t bytes[WM_CLIENT_INPUT_READ_QUANTUM];
			ssize_t count = -1;

			if (client->fd < 0)
				continue;
			if (pollfds[poll_index].revents & (POLLIN | POLLHUP | POLLERR)) {
				count = recv(client->fd, bytes, sizeof(bytes), 0);
				if (count > 0 && gui_protocol_input_append(client->input,
									 bytes, count) < 0)
					client->hello = -1;
				if (count == 0 ||
				    (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
					client->hello = -1;
			}
			if (client->fd >= 0 && client->hello >= 0 &&
			    (count > 0 || client->drain_input)) {
				int messages = gui_protocol_input_drain(client->input,
					client_message, client);

				if (messages < 0 || client_flush(client) < 0)
					client->hello = -1;
				/* Retry only a full drain quantum; partial frames must wait for POLLIN. */
				client->drain_input = messages == 8 &&
					gui_protocol_input_pending(client->input);
			}
			if (client->fd >= 0 &&
			    (pollfds[poll_index].revents & POLLOUT) &&
			    client_flush(client) < 0)
				client->hello = -1;
			/* A socket hangup is terminal even if a nonblocking recv() saw EAGAIN. */
			if (pollfds[poll_index].revents & (POLLHUP | POLLERR | POLLNVAL))
				client->hello = -1;
			if (client->hello < 0)
				wm_disconnect_client(&server, client);
			if (wm_present_motion(&server) < 0)
				goto out;
		}
		if (pollfds[2].revents & POLLIN) {
			int fd = accept4(server.state.listener, NULL, NULL, SOCK_NONBLOCK);

			if (fd >= 0) {
				/* A public peer can never acquire the private shell role. */
				(void)wm_add_client(&server, fd,
					GUI_PROTOCOL_ENDPOINT_ORDINARY);
			}
		}
		if (server.client_count < server.poll_capacity / 2)
			(void)wm_poll_resize(&server, server.client_count);
		if (!server.state.scanout_busy && server.state.damage.count != 0 &&
		    wm_compose(&server) == GUI_FLUSH_ERROR)
			goto out;
	}
	status = 0;
out:
	wm_stop_shell(&server);
	wm_discard_shell_list(&server);
	while (server.clients != NULL)
		client_destroy(server.clients);
	while (server.launches != NULL) {
		wm_launch *launch = server.launches;

		server.launches = launch->next;
		wm_record_destroy(&server, NULL, launch, sizeof(*launch));
	}
	(void)wm_poll_resize(&server, 0);
	if (server.desktop_snapshot != NULL) {
		server.state.surface_bytes -= server.desktop_snapshot_bytes;
		gui_surface_destroy(server.desktop_snapshot);
	}
	if (status != 0)
		perror("mario-wm");
	wm_cleanup(&server.state);
	return status;
}
