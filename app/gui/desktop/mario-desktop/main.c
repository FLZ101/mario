#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/input.h>
#include <gui/protocol.h>

#define WM_FD 3
#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define SURFACE_PIXELS_PER_MESSAGE 1000U
#define DESKTOP_BACKGROUND 0x001f2933U
#define DESKTOP_PANEL 0x00111827U
#define DESKTOP_LAUNCHER 0x002563ebU
#define DESKTOP_TASK_ACTIVE 0x002563ebU
#define DESKTOP_TASK_INACTIVE 0x00475569U
#define DESKTOP_TEXT 0x00f8fafcU
#define DESKTOP_PANEL_HEIGHT 28U
#define DESKTOP_WINDOW_LIMIT 8U
#define DESKTOP_TASKBAR_FIRST_X 64U
#define DESKTOP_TASKBAR_BUTTON_WIDTH 96U
#define DESKTOP_TASKBAR_BUTTON_STRIDE 100U
#define DESKTOP_TASKBAR_BUTTON_HEIGHT 20U
#define DIAGNOSTIC_WELCOME 0U
#define DIAGNOSTIC_DISPLAY 1U
#define DIAGNOSTIC_COUNT 2U

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	int32_t x;
	int32_t y;
	int redraw;
} diagnostic_client;

typedef struct {
	const char *title;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	int32_t x;
	int32_t y;
} diagnostic_definition;

static const diagnostic_definition diagnostic_definitions[DIAGNOSTIC_COUNT] = {
	[DIAGNOSTIC_WELCOME] = {
		.title = "Welcome", .width = 360, .height = 220,
		.color = 0x001d4ed8U, .x = 96, .y = 96,
	},
	[DIAGNOSTIC_DISPLAY] = {
		.title = "Display", .width = 320, .height = 192,
		.color = 0x000f766eU, .x = 480, .y = 220,
	},
};

typedef struct {
	uint32_t window_id;
	uint32_t focused;
	uint32_t order;
} desktop_window;

typedef struct {
	uint32_t window_id;
	uint64_t configure_serial;
	uint32_t width;
	uint32_t height;
	uint64_t frame_serial;
	uint8_t mouse_buttons;
	uint8_t launch_diagnostic;
	int redraw;
	desktop_window windows[DESKTOP_WINDOW_LIMIT];
} desktop_state;

/* Keep the largest wire frame out of the small initial userspace stack. */
static uint8_t desktop_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) +
	SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];

static void read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			_exit(1);
		bytes += count;
		byte_count -= count;
	}
}

static int write_full(int fd, const void *buffer, size_t byte_count)
{
	const uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = write(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		bytes += count;
		byte_count -= count;
	}
	return 0;
}

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put64(uint8_t *bytes, uint64_t value)
{
	for (unsigned int index = 0; index < 8; index++)
		bytes[index] = value >> (index * 8);
}

static uint32_t get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t get64(const uint8_t *bytes)
{
	uint64_t value = 0;

	for (unsigned int index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
}

static int glyph_pixel(char character, unsigned int column, unsigned int row)
{
	static const uint8_t glyphs[][5] = {
		/* - */ { 0x00, 0x00, 0x07, 0x00, 0x00 },
		/* : */ { 0x00, 0x02, 0x00, 0x02, 0x00 },
		/* E */ { 0x07, 0x04, 0x06, 0x04, 0x07 },
		/* M */ { 0x05, 0x07, 0x07, 0x05, 0x05 },
		/* N */ { 0x05, 0x07, 0x07, 0x07, 0x05 },
		/* U */ { 0x05, 0x05, 0x05, 0x05, 0x07 },
	};
	const uint8_t *glyph;

	if (column >= 3 || row >= 5)
		return 0;
	switch (character) {
	case '-': glyph = glyphs[0]; break;
	case ':': glyph = glyphs[1]; break;
	case 'E': glyph = glyphs[2]; break;
	case 'M': glyph = glyphs[3]; break;
	case 'N': glyph = glyphs[4]; break;
	case 'U': glyph = glyphs[5]; break;
	default: return 0;
	}
	return glyph[row] & (1U << (2 - column));
}

static int text_pixel(const char *text, uint32_t x, uint32_t y,
		      uint32_t origin_x, uint32_t origin_y)
{
	uint32_t character_index;

	if (x < origin_x || y < origin_y || y >= origin_y + 10)
		return 0;
	character_index = (x - origin_x) / 8;
	if (character_index >= strlen(text))
		return 0;
	return glyph_pixel(text[character_index],
		((x - origin_x) % 8) / 2, (y - origin_y) / 2);
}

static desktop_window *desktop_window_by_id(desktop_state *state,
					     uint32_t window_id)
{
	for (size_t index = 0; index < DESKTOP_WINDOW_LIMIT; index++) {
		if (state->windows[index].window_id == window_id)
			return &state->windows[index];
	}
	return NULL;
}

static const desktop_window *desktop_window_at(const desktop_state *state,
						uint32_t slot)
{
	uint32_t previous = 0;

	for (;;) {
		const desktop_window *next = NULL;

		for (size_t index = 0; index < DESKTOP_WINDOW_LIMIT; index++) {
			const desktop_window *window = &state->windows[index];

			if (window->window_id != 0 && window->window_id > previous &&
			    (next == NULL || window->window_id < next->window_id))
				next = window;
		}
		if (next == NULL)
			return NULL;
		if (slot == 0)
			return next;
		previous = next->window_id;
		slot--;
	}
}

static const desktop_window *desktop_taskbar_window(const desktop_state *state,
						     uint32_t x, uint32_t y)
{
	uint32_t panel_y = state->height > DESKTOP_PANEL_HEIGHT ?
		state->height - DESKTOP_PANEL_HEIGHT : 0;
	uint32_t clock_x = state->width > 40 ? state->width - 40 : 0;
	uint32_t slot;

	if (y < panel_y + 4 || y >= panel_y + 4 + DESKTOP_TASKBAR_BUTTON_HEIGHT ||
	    x < DESKTOP_TASKBAR_FIRST_X || x >= clock_x)
		return NULL;
	slot = (x - DESKTOP_TASKBAR_FIRST_X) / DESKTOP_TASKBAR_BUTTON_STRIDE;
	if (x - DESKTOP_TASKBAR_FIRST_X - slot * DESKTOP_TASKBAR_BUTTON_STRIDE >=
	    DESKTOP_TASKBAR_BUTTON_WIDTH)
		return NULL;
	return desktop_window_at(state, slot);
}

static uint32_t desktop_pixel(const desktop_state *state, uint32_t offset)
{
	uint32_t x = offset % state->width;
	uint32_t y = offset / state->width;
	uint32_t panel_y = state->height > DESKTOP_PANEL_HEIGHT ?
		state->height - DESKTOP_PANEL_HEIGHT : 0;
	const desktop_window *window;

	if (y < panel_y)
		return DESKTOP_BACKGROUND;
	if (x >= 8 && x < 56 && y >= panel_y + 4 && y < panel_y + 24) {
		if (text_pixel("MENU", x, y, 12, panel_y + 9))
			return DESKTOP_TEXT;
		return DESKTOP_LAUNCHER;
	}
	window = desktop_taskbar_window(state, x, y);
	if (window != NULL)
		return window->focused ? DESKTOP_TASK_ACTIVE : DESKTOP_TASK_INACTIVE;
	if (state->width >= 40 &&
	    text_pixel("--:--", x, y, state->width - 40, panel_y + 9))
		return DESKTOP_TEXT;
	return DESKTOP_PANEL;
}

static int read_message(int fd, gui_protocol_header *header, uint8_t *payload,
				size_t payload_capacity)
{
	uint8_t wire_header[GUI_PROTOCOL_HEADER_SIZE];

	read_full(fd, wire_header, sizeof(wire_header));
	if (gui_protocol_decode_header(wire_header, sizeof(wire_header), header) < 0 ||
	    header->length < GUI_PROTOCOL_HEADER_SIZE ||
	    header->length - GUI_PROTOCOL_HEADER_SIZE > payload_capacity)
		return -1;
	read_full(fd, payload, header->length - GUI_PROTOCOL_HEADER_SIZE);
	return 0;
}

static int desktop_read_message(gui_protocol_header *header, uint8_t *payload,
					size_t payload_capacity)
{
	return read_message(WM_FD, header, payload, payload_capacity);
}

static int desktop_activate_window(uint32_t window_id)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_id)];

	if (gui_protocol_encode_header(frame, sizeof(frame),
		GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW,
		sizeof(gui_protocol_wire_window_id)) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, window_id);
	return write_full(WM_FD, frame, sizeof(frame));
}

static int send_message(int fd, gui_protocol_type type, const void *payload,
			 size_t payload_bytes)
{
	uint8_t header[GUI_PROTOCOL_HEADER_SIZE];

	if (gui_protocol_encode_header(header, sizeof(header), type, payload_bytes) < 0 ||
	    write_full(fd, header, sizeof(header)) < 0 ||
	    (payload_bytes != 0 && write_full(fd, payload, payload_bytes) < 0))
		return -1;
	return 0;
}

static int desktop_handle_window_list(desktop_state *state,
				      gui_protocol_type type, const uint8_t *payload,
				      size_t payload_bytes)
{
	desktop_window *window;
	uint32_t window_id;

	if (payload_bytes < sizeof(gui_protocol_wire_window_list))
		return -1;
	window_id = get32(payload);
	if (type == GUI_PROTOCOL_WINDOW_LIST_REMOVE) {
		window = desktop_window_by_id(state, window_id);
		if (window == NULL)
			return -1;
		*window = (desktop_window) { 0 };
		state->redraw = 1;
		return 0;
	}
	window = desktop_window_by_id(state, window_id);
	if (type == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT ||
	    type == GUI_PROTOCOL_WINDOW_LIST_ADD) {
		if (window == NULL) {
			for (size_t index = 0; index < DESKTOP_WINDOW_LIMIT; index++) {
				if (state->windows[index].window_id == 0) {
					window = &state->windows[index];
					break;
				}
			}
		}
		if (window == NULL)
			return -1;
	} else if (window == NULL) {
		return -1;
	}
	window->window_id = window_id;
	window->focused = get32(payload + 20);
	window->order = get32(payload + 24);
	state->redraw = 1;
	return 0;
}

static int desktop_handle_message(desktop_state *state,
				  const gui_protocol_header *header,
				  const uint8_t *payload, size_t payload_bytes)
{
	if (header->type == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT ||
	    header->type == GUI_PROTOCOL_WINDOW_LIST_ADD ||
	    header->type == GUI_PROTOCOL_WINDOW_LIST_CHANGE ||
	    header->type == GUI_PROTOCOL_WINDOW_LIST_REMOVE)
		return desktop_handle_window_list(state, header->type, payload, payload_bytes);
	if (header->type == GUI_PROTOCOL_INPUT_RESET) {
		if (payload_bytes != 0)
			return -1;
		state->mouse_buttons = 0;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		uint8_t buttons;
		uint32_t x;
		uint32_t y;
		const desktop_window *window;

		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != state->window_id ||
		    get32(payload + 4) != GUI_INPUT_EVENT_MOUSE)
			return -1;
		x = get32(payload + 16);
		y = get32(payload + 20);
		buttons = payload[24];
		if (!(state->mouse_buttons & 1) && (buttons & 1) &&
		    x >= 8 && x < 56 && y >= (state->height > DESKTOP_PANEL_HEIGHT ?
		    state->height - DESKTOP_PANEL_HEIGHT : 0) + 4 &&
		    y < (state->height > DESKTOP_PANEL_HEIGHT ?
		    state->height - DESKTOP_PANEL_HEIGHT : 0) + 24) {
			state->launch_diagnostic = x < 32 ? DIAGNOSTIC_WELCOME + 1 :
				DIAGNOSTIC_DISPLAY + 1;
			state->mouse_buttons = buttons;
			return 0;
		}
		window = desktop_taskbar_window(state, x, y);
		if (!(state->mouse_buttons & 1) && (buttons & 1) && window != NULL &&
		    desktop_activate_window(window->window_id) < 0)
			return -1;
		state->mouse_buttons = buttons;
		return 0;
	}
	return -1;
}

static int read_surface_data_reply(desktop_state *state, uint64_t frame_serial,
				   uint32_t pixel_offset, uint32_t pixel_count)
{
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];

	for (;;) {
		gui_protocol_header header;

		if (desktop_read_message(&header, payload, sizeof(payload)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(payload) != state->window_id ||
			    get64(payload + 4) != state->configure_serial ||
			    get64(payload + 12) != frame_serial ||
			    get32(payload + 20) != pixel_offset ||
			    get32(payload + 24) != pixel_count)
				return -1;
			return 0;
		}
		if (desktop_handle_message(state, &header, payload,
			    header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)
			return -1;
	}
}

static int send_desktop(desktop_state *state)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++state->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || (state->width != 0 &&
	    state->height > UINT32_MAX / state->width))
		return -1;
	total = state->width * state->height;

	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
		sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + 8, state->window_id);
	put64(begin + 12, state->configure_serial);
	put64(begin + 20, frame_serial);
	put32(begin + 36, state->width); put32(begin + 40, state->height);
	if (write_full(WM_FD, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixels = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload = sizeof(gui_protocol_wire_surface_data) +
			pixels * sizeof(uint32_t);

		/* The protocol keeps each frame below Mario's one-page socket ring. */
		if (gui_protocol_encode_header(desktop_frame, sizeof(desktop_frame),
			GUI_PROTOCOL_SURFACE_DATA, payload) < 0)
			return -1;
		put32(desktop_frame + 8, state->window_id);
		put64(desktop_frame + 12, state->configure_serial);
		put64(desktop_frame + 20, frame_serial);
		put32(desktop_frame + 28, pixel_offset); put32(desktop_frame + 32, pixels);
		/* Build the complete shell scene incrementally to stay within the socket ring. */
		for (uint32_t index = 0; index < pixels; index++)
			put32(desktop_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				desktop_pixel(state, pixel_offset + index));
		if (write_full(WM_FD, desktop_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload) < 0 ||
		    read_surface_data_reply(state, frame_serial, pixel_offset, pixels) < 0)
			return -1;
		pixel_offset += pixels;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
		sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + 8, state->window_id);
	put64(commit + 12, state->configure_serial);
	put64(commit + 20, frame_serial);
	return write_full(WM_FD, commit, sizeof(commit));
}

static void diagnostic_reset(diagnostic_client *client)
{
	if (client->fd >= 0)
		close(client->fd);
	*client = (diagnostic_client) { .fd = -1 };
}

static int diagnostic_handle_message(diagnostic_client *client,
				     const gui_protocol_header *header,
				     const uint8_t *payload, size_t payload_bytes)
{
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != client->window_id || get64(payload + 4) == 0 ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0)
			return -1;
		client->configure_serial = get64(payload + 4);
		client->width = get32(payload + 12);
		client->height = get32(payload + 16);
		client->redraw = 1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST) {
		uint8_t destroy[sizeof(gui_protocol_wire_window_id)];

		if (payload_bytes != sizeof(destroy) || get32(payload) != client->window_id)
			return -1;
		put32(destroy, client->window_id);
		if (send_message(client->fd, GUI_PROTOCOL_WINDOW_DESTROY, destroy,
				 sizeof(destroy)) < 0)
			return -1;
		/* Closing the ordinary endpoint makes launcher recreation unambiguous. */
		diagnostic_reset(client);
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_FOCUS) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_focus) ||
		    get32(payload) != client->window_id || get32(payload + 4) > 1)
			return -1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_INPUT_RESET)
		return payload_bytes == 0 ? 0 : -1;
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != client->window_id)
			return -1;
		return 0;
	}
	return -1;
}

static int diagnostic_read_surface_reply(diagnostic_client *client,
					 uint64_t frame_serial, uint32_t pixel_offset,
					 uint32_t pixel_count)
{
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];

	for (;;) {
		gui_protocol_header header;

		if (read_message(client->fd, &header, payload, sizeof(payload)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(payload) != client->window_id ||
			    get64(payload + 4) != client->configure_serial ||
			    get64(payload + 12) != frame_serial ||
			    get32(payload + 20) != pixel_offset ||
			    get32(payload + 24) != pixel_count)
				return -1;
			return 0;
		}
		if (diagnostic_handle_message(client, &header, payload,
			    header.length - GUI_PROTOCOL_HEADER_SIZE) < 0 || client->fd < 0)
			return -1;
	}
}

static int send_diagnostic(diagnostic_client *client)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++client->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || (client->width != 0 &&
	    client->height > UINT32_MAX / client->width))
		return -1;
	total = client->width * client->height;
	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
		sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + 8, client->window_id);
	put64(begin + 12, client->configure_serial);
	put64(begin + 20, frame_serial);
	put32(begin + 36, client->width); put32(begin + 40, client->height);
	if (write_full(client->fd, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixels = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload = sizeof(gui_protocol_wire_surface_data) +
			pixels * sizeof(uint32_t);

		if (gui_protocol_encode_header(desktop_frame, sizeof(desktop_frame),
			GUI_PROTOCOL_SURFACE_DATA, payload) < 0)
			return -1;
		put32(desktop_frame + 8, client->window_id);
		put64(desktop_frame + 12, client->configure_serial);
		put64(desktop_frame + 20, frame_serial);
		put32(desktop_frame + 28, pixel_offset); put32(desktop_frame + 32, pixels);
		for (uint32_t index = 0; index < pixels; index++)
			put32(desktop_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				client->color);
		if (write_full(client->fd, desktop_frame, GUI_PROTOCOL_HEADER_SIZE + payload) < 0 ||
		    diagnostic_read_surface_reply(client, frame_serial, pixel_offset, pixels) < 0)
			return -1;
		pixel_offset += pixels;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
		sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + 8, client->window_id);
	put64(commit + 12, client->configure_serial);
	put64(commit + 20, frame_serial);
	return write_full(client->fd, commit, sizeof(commit));
}

static int diagnostic_connect(diagnostic_client *client, unsigned int index)
{
	const diagnostic_definition *definition = &diagnostic_definitions[index];
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		GUI_PROTOCOL_MAX_TITLE_BYTES] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];
	gui_protocol_header header;
	size_t title_bytes = strlen(definition->title);
	size_t create_bytes = sizeof(gui_protocol_wire_window_create) + title_bytes;

	if (title_bytes > GUI_PROTOCOL_MAX_TITLE_BYTES ||
	    (client->fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	strcpy(address.sun_path, WM_SOCKET_PATH);
	if (connect(client->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(client->fd, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0 ||
	    read_message(client->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		goto fail;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
		create_bytes) < 0)
		goto fail;
	put32(frame + 8, index + 1);
	put32(frame + 12, definition->width);
	put32(frame + 16, definition->height);
	frame[20] = (uint8_t)title_bytes;
	frame[21] = (uint8_t)(title_bytes >> 8);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		definition->title, title_bytes);
	if (write_full(client->fd, frame, GUI_PROTOCOL_HEADER_SIZE + create_bytes) < 0 ||
	    read_message(client->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_WINDOW_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_window_create_reply) || get32(payload) != index + 1)
		goto fail;
	client->window_id = get32(payload + 4);
	client->configure_serial = get64(payload + 8);
	client->width = get32(payload + 16);
	client->height = get32(payload + 20);
	client->color = definition->color;
	client->x = definition->x;
	client->y = definition->y;
	if (client->window_id == 0 || client->configure_serial == 0 ||
	    client->width == 0 || client->height == 0)
		goto fail;
	/* Creation focus and reset are ordered before this client can draw. */
	for (unsigned int count = 0; count < 2; count++) {
		if (read_message(client->fd, &header, payload, sizeof(payload)) < 0 ||
		    diagnostic_handle_message(client, &header, payload,
			header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)
			goto fail;
	}
	put32(frame, client->window_id);
	put32(frame + 4, (uint32_t)client->x);
	put32(frame + 8, (uint32_t)client->y);
	if (send_message(client->fd, GUI_PROTOCOL_WINDOW_MOVE, frame,
		sizeof(gui_protocol_wire_window_move)) < 0 || send_diagnostic(client) < 0)
		goto fail;
	return 0;

fail:
	diagnostic_reset(client);
	return -1;
}

static int desktop_launch_diagnostic(desktop_state *state,
				     diagnostic_client *diagnostics, unsigned int index)
{
	diagnostic_client *client = &diagnostics[index];

	if (client->fd >= 0)
		return desktop_activate_window(client->window_id);
	if (diagnostic_connect(client, index) < 0)
		return -1;
	state->redraw = 1;
	return 0;
}

static int private_fd_only(void)
{
	if (fcntl(WM_FD, F_GETFD) < 0)
		return -1;
	for (int fd = WM_FD + 1; fd < 1024; fd++) {
		if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
			return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	desktop_state state = { 0 };
	diagnostic_client diagnostics[DIAGNOSTIC_COUNT];
	uint8_t frame[64];
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];
	gui_protocol_header header;
	struct pollfd pollfds[DIAGNOSTIC_COUNT + 1];

	for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++)
		diagnostics[index].fd = -1;

	if (argc != 2 || strcmp(argv[1], "--wm-fd=3") != 0 || private_fd_only() < 0) {
		fprintf(stderr, "mario-desktop: invalid inherited descriptors\n");
		return 1;
	}
	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0)
		return 1;
	if (desktop_read_message(&header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		return 1;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_DESKTOP_CREATE,
		sizeof(gui_protocol_wire_desktop_create)) < 0)
		return 1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	if (write_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_desktop_create)) < 0)
		return 1;
	if (desktop_read_message(&header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_DESKTOP_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_desktop_create_reply))
		return 1;
	state.window_id = get32(payload + 4);
	state.configure_serial = get64(payload + 8);
	state.width = get32(payload + 16);
	state.height = get32(payload + 20);
	if (state.window_id == 0 || state.configure_serial == 0)
		return 1;
	if (send_desktop(&state) < 0)
		return 1;
	for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
		if (diagnostic_connect(&diagnostics[index], index) < 0)
			return 1;
	}
	/* The WM may have forked after another shell wrote stdout. */
	if (write_full(STDOUT_FILENO, "mario-desktop: private connection ready\n",
		       sizeof("mario-desktop: private connection ready\n") - 1) < 0)
		return 1;
	for (;;) {
		if (state.redraw) {
			state.redraw = 0;
			if (send_desktop(&state) < 0)
				return 1;
			continue;
		}
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			if (diagnostics[index].fd >= 0 && diagnostics[index].redraw) {
				diagnostics[index].redraw = 0;
				if (send_diagnostic(&diagnostics[index]) < 0)
					return 1;
				break;
			}
		}
		if (state.launch_diagnostic != 0) {
			unsigned int index = state.launch_diagnostic - 1;

			state.launch_diagnostic = 0;
			if (index >= DIAGNOSTIC_COUNT ||
			    desktop_launch_diagnostic(&state, diagnostics, index) < 0)
				return 1;
			continue;
		}
		pollfds[0] = (struct pollfd) { .fd = WM_FD, .events = POLLIN };
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			pollfds[index + 1] = (struct pollfd) {
				.fd = diagnostics[index].fd,
				.events = diagnostics[index].fd >= 0 ? POLLIN : 0,
			};
		}
		if (poll(pollfds, DIAGNOSTIC_COUNT + 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL) ||
		    (pollfds[0].revents & POLLIN &&
		     (desktop_read_message(&header, payload, sizeof(payload)) < 0 ||
		      desktop_handle_message(&state, &header, payload,
			header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)))
			return 1;
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			if (!(pollfds[index + 1].revents & POLLIN))
				continue;
			if (read_message(diagnostics[index].fd, &header, payload,
			    sizeof(payload)) < 0 || diagnostic_handle_message(&diagnostics[index],
			    &header, payload, header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)
				return 1;
		}
	}
}
