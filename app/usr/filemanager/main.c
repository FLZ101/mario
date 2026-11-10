#include "model.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/protocol.h>
#include <gui/font.h>
#include <gui/input.h>
#include <gui/surface.h>

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define FILEMANAGER_WIDTH 320U
#define FILEMANAGER_HEIGHT 192U
#define FILEMANAGER_TITLE "File Manager"
#define FILEMANAGER_BACKGROUND 0x00111827U
#define FILEMANAGER_FOREGROUND 0x00f8fafcU
#define FILEMANAGER_HEADER 0x0094a3b8U
#define FILEMANAGER_SELECTION 0x001d4ed8U
#define FILEMANAGER_FONT_PATH "/usr/share/fonts/DejaVuSansMono.ttf"
#define FILEMANAGER_FONT_HEIGHT 14
#define FILEMANAGER_MARGIN 6
#define FILEMANAGER_HEADER_Y 6
#define FILEMANAGER_COLUMNS_Y 24
#define FILEMANAGER_ROWS_Y 24
#define FILEMANAGER_PATH_ROWS_Y 42
#define FILEMANAGER_ROW_HEIGHT 16
#define FILEMANAGER_NAME_X 6
#define FILEMANAGER_TYPE_X 112
#define FILEMANAGER_SIZE_X 170
#define FILEMANAGER_TIME_X 228
#define FILEMANAGER_PATH_CHARS 35U
#define FILEMANAGER_PATH_VALUE_X 56
#define FILEMANAGER_NAME_CHARS 13U
#define FILEMANAGER_TYPE_CHARS 7U
#define FILEMANAGER_SIZE_CHARS 7U
#define FILEMANAGER_TIME_CHARS 10U
#define SURFACE_PIXELS_PER_MESSAGE 1000U

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	int close_requested;
	size_t selected_entry;
	int path_entry_active;
	char *path;
	char *path_entry;
	char status[32];
	gui_font *font;
	gui_surface *surface;
} filemanager_state;

/* Keep protocol frames off Mario's intentionally small initial stack. */
static uint8_t filemanager_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) + SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];
static uint8_t filemanager_input[GUI_PROTOCOL_MAX_MESSAGE_SIZE - GUI_PROTOCOL_HEADER_SIZE];
/* The bounded listing is larger than Mario's initial stack budget. */
static filemanager_directory filemanager_directory_model;
/* Path input is also bounded but should not inflate the initial application stack. */
static char filemanager_path[GUI_PROTOCOL_MAX_PATH_BYTES + 1];
static char filemanager_path_entry[GUI_PROTOCOL_MAX_PATH_BYTES + 1];

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static uint16_t get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
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

static int read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		bytes += count;
		byte_count -= count;
	}
	return 0;
}

static int send_message(int fd, gui_protocol_type type, const void *payload,
				size_t payload_bytes)
{
	uint8_t header[GUI_PROTOCOL_HEADER_SIZE];

	return gui_protocol_encode_header(header, sizeof(header), type, payload_bytes) < 0 ||
		write_full(fd, header, sizeof(header)) < 0 ||
		(payload_bytes != 0 && write_full(fd, payload, payload_bytes) < 0) ? -1 : 0;
}

static int read_message(int fd, gui_protocol_header *header, uint8_t *payload,
				size_t payload_capacity)
{
	uint8_t wire_header[GUI_PROTOCOL_HEADER_SIZE];

	if (read_full(fd, wire_header, sizeof(wire_header)) < 0 ||
	    gui_protocol_decode_header(wire_header, sizeof(wire_header), header) < 0 ||
	    header->length < GUI_PROTOCOL_HEADER_SIZE ||
	    header->length - GUI_PROTOCOL_HEADER_SIZE > payload_capacity)
		return -1;
	return read_full(fd, payload, header->length - GUI_PROTOCOL_HEADER_SIZE);
}

static int filemanager_resize_surface(filemanager_state *state, uint32_t width,
				      uint32_t height)
{
	gui_surface *surface;

	if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX)
		return -1;
	surface = gui_surface_create((int)width, (int)height);
	if (surface == NULL)
		return -1;
	gui_surface_destroy(state->surface);
	state->surface = surface;
	state->width = width;
	state->height = height;
	return 0;
}

static const char *filemanager_type_name(filemanager_entry_type type)
{
	switch (type) {
	case FILEMANAGER_ENTRY_DIRECTORY: return "dir";
	case FILEMANAGER_ENTRY_FILE: return "file";
	case FILEMANAGER_ENTRY_SYMLINK: return "link";
	case FILEMANAGER_ENTRY_OTHER: return "other";
	case FILEMANAGER_ENTRY_UNKNOWN: return "unknown";
	}
	return "unknown";
}

static size_t filemanager_utf8_bytes(const char *text)
{
	unsigned char first = (unsigned char)text[0];
	size_t bytes = 1;

	if ((first & 0xe0) == 0xc0)
		bytes = 2;
	else if ((first & 0xf0) == 0xe0)
		bytes = 3;
	else if ((first & 0xf8) == 0xf0)
		bytes = 4;
	for (size_t index = 1; index < bytes; index++)
		if ((unsigned char)text[index] == 0 || ((unsigned char)text[index] & 0xc0) != 0x80)
			return 1;
	return bytes;
}

static size_t filemanager_utf8_count(const char *text, size_t limit)
{
	size_t count = 0;

	while (*text != '\0' && count < limit) {
		text += filemanager_utf8_bytes(text);
		count++;
	}
	return count;
}

static void filemanager_clip_text(char *destination, size_t destination_bytes,
				  const char *source, size_t character_limit)
{
	size_t source_count = filemanager_utf8_count(source, character_limit + 1);
	size_t kept = source_count > character_limit ? character_limit - 3 : source_count;
	size_t written = 0;

	while (*source != '\0' && kept != 0) {
		size_t bytes = filemanager_utf8_bytes(source);

		if (written + bytes + 1 > destination_bytes)
			break;
		memcpy(destination + written, source, bytes);
		written += bytes;
		source += bytes;
		kept--;
	}
	if (source_count > character_limit && written + 4 <= destination_bytes) {
		memcpy(destination + written, "...", 3);
		written += 3;
	}
	destination[written] = '\0';
}

static int filemanager_validate_path(const char *path)
{
	size_t path_bytes;

	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
	path_bytes = strnlen(path, GUI_PROTOCOL_MAX_PATH_BYTES + 1);
	if (path[0] != '/' || path_bytes == 0 || path_bytes > GUI_PROTOCOL_MAX_PATH_BYTES) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static int filemanager_parent_path(char *destination, size_t destination_bytes,
				   const char *path)
{
	size_t path_bytes;

	if (destination == NULL || destination_bytes < GUI_PROTOCOL_MAX_PATH_BYTES + 1 ||
	    filemanager_validate_path(path) < 0)
		return -1;
	path_bytes = strlen(path);
	while (path_bytes > 1 && path[path_bytes - 1] == '/')
		path_bytes--;
	while (path_bytes > 1 && path[path_bytes - 1] != '/')
		path_bytes--;
	if (path_bytes > 1)
		path_bytes--;
	memcpy(destination, path, path_bytes);
	destination[path_bytes] = '\0';
	return 0;
}

static int filemanager_child_path(char *destination, size_t destination_bytes,
				  const char *path, const char *name)
{
	size_t path_bytes;
	size_t name_bytes;

	if (destination == NULL || name == NULL ||
	    destination_bytes < GUI_PROTOCOL_MAX_PATH_BYTES + 1 ||
	    filemanager_validate_path(path) < 0)
		return -1;
	path_bytes = strlen(path);
	while (path_bytes > 1 && path[path_bytes - 1] == '/')
		path_bytes--;
	name_bytes = strnlen(name, FILEMANAGER_NAME_BYTES + 1);
	if (name_bytes == 0 || name_bytes > FILEMANAGER_NAME_BYTES ||
	    path_bytes + (path_bytes == 1 ? 0 : 1) + name_bytes > GUI_PROTOCOL_MAX_PATH_BYTES) {
		errno = ENAMETOOLONG;
		return -1;
	}
	memcpy(destination, path, path_bytes);
	if (path_bytes != 1)
		destination[path_bytes++] = '/';
	memcpy(destination + path_bytes, name, name_bytes);
	destination[path_bytes + name_bytes] = '\0';
	return 0;
}

static int filemanager_navigate(filemanager_state *state, const char *path)
{
	int saved_errno;

	if (filemanager_validate_path(path) < 0 ||
	    filemanager_directory_load(&filemanager_directory_model, path) < 0) {
		saved_errno = errno;
		(void)snprintf(state->status, sizeof(state->status), "Path error: %d", saved_errno);
		fprintf(stderr, "filemanager: navigation failed: %s: %d\n", path, saved_errno);
		fflush(stderr);
		return 0;
	}
	/* The model commits only after a complete load, so path and rows change together. */
	strcpy(state->path, path);
	state->selected_entry = 0;
	state->path_entry_active = 0;
	state->path_entry[0] = '\0';
	state->status[0] = '\0';
	printf("filemanager: navigated: %s (%zu entries%s)\n", state->path,
		filemanager_directory_model.count,
		filemanager_directory_model.omitted_count == 0 ? "" : ", truncated");
	fflush(stdout);
	return 1;
}

static int filemanager_handle_path_entry(filemanager_state *state, uint32_t keysym)
{
	size_t path_bytes;

	if (keysym == GUI_KEYSYM_ESCAPE) {
		state->path_entry_active = 0;
		state->path_entry[0] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	path_bytes = strlen(state->path_entry);
	if (keysym == GUI_KEYSYM_BACKSPACE) {
		if (path_bytes == 0)
			return 0;
		state->path_entry[path_bytes - 1] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	if (keysym == '\n') {
		(void)filemanager_navigate(state, state->path_entry);
		return 1;
	}
	if (keysym >= 0x20 && keysym <= 0x7e && path_bytes < GUI_PROTOCOL_MAX_PATH_BYTES) {
		state->path_entry[path_bytes] = (char)keysym;
		state->path_entry[path_bytes + 1] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	return 0;
}

static int filemanager_handle_key(filemanager_state *state, uint32_t keysym)
{
	if (state->path_entry_active)
		return filemanager_handle_path_entry(state, keysym);
	if (keysym == '/') {
		state->path_entry_active = 1;
		state->path_entry[0] = '\0';
		state->status[0] = '\0';
		return 1;
	}
	if (keysym == GUI_KEYSYM_BACKSPACE || keysym == GUI_KEYSYM_LEFT || keysym == 'h') {
		if (filemanager_parent_path(state->path_entry, sizeof(filemanager_path_entry),
			state->path) < 0 || !strcmp(state->path_entry, state->path))
			return 0;
		(void)filemanager_navigate(state, state->path_entry);
		return 1;
	}
	if (keysym == '\n' && state->selected_entry < filemanager_directory_model.count) {
		/* The transactional load is the final directory check and retains rows on error. */
		if (filemanager_child_path(state->path_entry, sizeof(filemanager_path_entry), state->path,
			filemanager_directory_model.entries[state->selected_entry].name) < 0)
			return 0;
		(void)filemanager_navigate(state, state->path_entry);
		return 1;
	}
	if (filemanager_directory_model.count == 0)
		return 0;
	if ((keysym == GUI_KEYSYM_UP || keysym == 'k') && state->selected_entry != 0) {
		state->selected_entry--;
		return 1;
	}
	if ((keysym == GUI_KEYSYM_DOWN || keysym == 'j') &&
	    state->selected_entry + 1 < filemanager_directory_model.count) {
		state->selected_entry++;
		return 1;
	}
	return 0;
}

static void filemanager_render(filemanager_state *state)
{
	char clipped_path[FILEMANAGER_PATH_CHARS * 4 + 1];
	int columns_y = FILEMANAGER_HEADER_Y;
	uint32_t rows_y = FILEMANAGER_ROWS_Y;
	uint32_t visible_rows;

	gui_fill_rect(state->surface, (gui_rect) { .w = (int)state->width,
		.h = (int)state->height }, FILEMANAGER_BACKGROUND);
	if (state->font == NULL)
		return;
	if (state->status[0] != '\0') {
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font, state->status,
			FILEMANAGER_FOREGROUND);
		columns_y = FILEMANAGER_COLUMNS_Y;
		rows_y = FILEMANAGER_PATH_ROWS_Y;
	} else if (state->path_entry_active) {
		filemanager_clip_text(clipped_path, sizeof(clipped_path), state->path_entry,
			FILEMANAGER_PATH_CHARS - 6);
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font, "Open:",
			FILEMANAGER_HEADER);
		gui_font_draw_text(state->surface, FILEMANAGER_PATH_VALUE_X,
			FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT, state->font, clipped_path,
			FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_PATH_VALUE_X +
			(int)strlen(clipped_path) * 8, FILEMANAGER_HEADER_Y + FILEMANAGER_FONT_HEIGHT,
			state->font, "_", FILEMANAGER_FOREGROUND);
		columns_y = FILEMANAGER_COLUMNS_Y;
		rows_y = FILEMANAGER_PATH_ROWS_Y;
	}
	gui_font_draw_text(state->surface, FILEMANAGER_NAME_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Name",
		FILEMANAGER_HEADER);
	gui_font_draw_text(state->surface, FILEMANAGER_TYPE_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Type",
		FILEMANAGER_HEADER);
	gui_font_draw_text(state->surface, FILEMANAGER_SIZE_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Size",
		FILEMANAGER_HEADER);
	gui_font_draw_text(state->surface, FILEMANAGER_TIME_X,
		columns_y + FILEMANAGER_FONT_HEIGHT, state->font, "Time",
		FILEMANAGER_HEADER);
	visible_rows = state->height <= rows_y ? 0 :
		(state->height - rows_y) / FILEMANAGER_ROW_HEIGHT;
	if (visible_rows > filemanager_directory_model.count)
		visible_rows = filemanager_directory_model.count;
	for (uint32_t row = 0; row < visible_rows; row++) {
		const filemanager_entry *entry = &filemanager_directory_model.entries[row];
		char name[FILEMANAGER_NAME_CHARS * 4 + 1];
		char type[FILEMANAGER_TYPE_CHARS * 4 + 1];
		char size[FILEMANAGER_SIZE_CHARS * 4 + 1];
		char time[FILEMANAGER_TIME_CHARS * 4 + 1];
		char size_text[32];
		char time_text[32];
		int y = (int)rows_y + (int)row * FILEMANAGER_ROW_HEIGHT;

		if (row == state->selected_entry)
			gui_fill_rect(state->surface, (gui_rect) { .x = FILEMANAGER_MARGIN - 2,
				.y = y, .w = (int)state->width - 2 * (FILEMANAGER_MARGIN - 2),
				.h = FILEMANAGER_ROW_HEIGHT }, FILEMANAGER_SELECTION);
		filemanager_clip_text(name, sizeof(name), entry->name, FILEMANAGER_NAME_CHARS);
		filemanager_clip_text(type, sizeof(type), filemanager_type_name(entry->type),
			FILEMANAGER_TYPE_CHARS);
		if (entry->metadata_available) {
			(void)snprintf(size_text, sizeof(size_text), "%llu",
				(unsigned long long)entry->size);
			(void)snprintf(time_text, sizeof(time_text), "%lld",
				(long long)entry->modified_time);
		} else {
			strcpy(size_text, "-");
			strcpy(time_text, "-");
		}
		filemanager_clip_text(size, sizeof(size), size_text, FILEMANAGER_SIZE_CHARS);
		filemanager_clip_text(time, sizeof(time), time_text, FILEMANAGER_TIME_CHARS);
		gui_font_draw_text(state->surface, FILEMANAGER_NAME_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, name, FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_TYPE_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, type, FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_SIZE_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, size, FILEMANAGER_FOREGROUND);
		gui_font_draw_text(state->surface, FILEMANAGER_TIME_X, y + FILEMANAGER_FONT_HEIGHT,
			state->font, time, FILEMANAGER_FOREGROUND);
	}
}

static int filemanager_handle_message(filemanager_state *state,
				      const gui_protocol_header *header,
				      const uint8_t *payload, size_t payload_bytes)
{
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != state->window_id || get64(payload + 4) == 0 ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0 ||
		    filemanager_resize_surface(state, get32(payload + 12), get32(payload + 16)) < 0)
			return -1;
		state->configure_serial = get64(payload + 4);
		return 1;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_id) ||
		    get32(payload) != state->window_id)
			return -1;
		state->close_requested = 1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_FOCUS)
		return payload_bytes == sizeof(gui_protocol_wire_window_focus) &&
			get32(payload) == state->window_id ? 0 : -1;
	if (header->type == GUI_PROTOCOL_INPUT_RESET)
		return payload_bytes == 0 ? 0 : -1;
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != state->window_id)
			return -1;
		return get32(payload + 4) == GUI_INPUT_EVENT_KEY ?
			filemanager_handle_key(state, get32(payload + 8)) : 0;
	}
	return -1;
}

static int filemanager_read_surface_reply(filemanager_state *state,
					  uint64_t frame_serial, uint32_t pixel_offset,
					  uint32_t pixel_count)
{
	for (;;) {
		gui_protocol_header header;
		int result;

		if (read_message(state->fd, &header, filemanager_input,
				 sizeof(filemanager_input)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(filemanager_input) != state->window_id ||
			    get64(filemanager_input + 4) != state->configure_serial ||
			    get64(filemanager_input + 12) != frame_serial ||
			    get32(filemanager_input + 20) != pixel_offset ||
			    get32(filemanager_input + 24) != pixel_count)
				return -1;
			return 0;
		}
		if (header.type == GUI_PROTOCOL_ERROR && header.length == GUI_PROTOCOL_HEADER_SIZE +
		    sizeof(gui_protocol_wire_error)) {
			fprintf(stderr, "filemanager: protocol rejected %u: %d\n",
				get16(filemanager_input + 4), (int32_t)get32(filemanager_input));
			errno = EPROTO;
			return -1;
		}
		result = filemanager_handle_message(state, &header, filemanager_input,
			header.length - GUI_PROTOCOL_HEADER_SIZE);
		if (result < 0 || state->close_requested)
			return -1;
		if (result > 0)
			return 1;
	}
}

static int filemanager_send_surface(filemanager_state *state)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++state->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || state->height > UINT32_MAX / state->width)
		return -1;
	filemanager_render(state);
	total = state->width * state->height;
	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
				       sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
	put64(begin + GUI_PROTOCOL_HEADER_SIZE + 4, state->configure_serial);
	put64(begin + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
	put32(begin + GUI_PROTOCOL_HEADER_SIZE + 28, state->width);
	put32(begin + GUI_PROTOCOL_HEADER_SIZE + 32, state->height);
	if (write_full(state->fd, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixel_count = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload_bytes = sizeof(gui_protocol_wire_surface_data) +
			pixel_count * sizeof(uint32_t);
		int result;

		if (gui_protocol_encode_header(filemanager_frame, sizeof(filemanager_frame),
				GUI_PROTOCOL_SURFACE_DATA, payload_bytes) < 0)
			return -1;
		put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
		put64(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 4,
			state->configure_serial);
		put64(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
		put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 20, pixel_offset);
		put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE + 24, pixel_count);
		for (uint32_t index = 0; index < pixel_count; index++)
			put32(filemanager_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				state->surface->pixels[pixel_offset + index]);
		if (write_full(state->fd, filemanager_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload_bytes) < 0)
			return -1;
		result = filemanager_read_surface_reply(state, frame_serial, pixel_offset,
			pixel_count);
		if (result != 0)
			return result;
		pixel_offset += pixel_count;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
				       sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
	put64(commit + GUI_PROTOCOL_HEADER_SIZE + 4, state->configure_serial);
	put64(commit + GUI_PROTOCOL_HEADER_SIZE + 12, frame_serial);
	return write_full(state->fd, commit, sizeof(commit));
}

static int filemanager_send_current_surface(filemanager_state *state)
{
	int result;

	/* A resize may arrive while the server acknowledges a surface chunk. */
	do {
		result = filemanager_send_surface(state);
		if (result < 0)
			return -1;
	} while (result > 0);
	return 0;
}

static int filemanager_connect(void)
{
	struct sockaddr_un address = { 0 };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0)
		return -1;
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, WM_SOCKET_PATH, sizeof(WM_SOCKET_PATH));
	if (connect(fd, (const struct sockaddr *)&address,
		sizeof(address.sun_family) + sizeof(WM_SOCKET_PATH)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int filemanager_create_window(filemanager_state *state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		sizeof(FILEMANAGER_TITLE) - 1] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_create_reply)];
	gui_protocol_header header;
	size_t title_bytes = sizeof(FILEMANAGER_TITLE) - 1;

	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(state->fd, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0 ||
	    read_message(state->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		return -1;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
		sizeof(gui_protocol_wire_window_create) + title_bytes) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 4, FILEMANAGER_WIDTH);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 8, FILEMANAGER_HEIGHT);
	put16(frame + GUI_PROTOCOL_HEADER_SIZE + 12, title_bytes);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		FILEMANAGER_TITLE, title_bytes);
	if (write_full(state->fd, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_window_create) + title_bytes) < 0 ||
	    read_message(state->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_WINDOW_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_window_create_reply) || get32(payload) != 1)
		return -1;
	state->window_id = get32(payload + 4);
	state->configure_serial = get64(payload + 8);
	state->width = get32(payload + 16);
	state->height = get32(payload + 20);
	return state->window_id != 0 && state->configure_serial != 0 &&
		state->width != 0 && state->height != 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
	filemanager_state state = { .fd = -1, .path = filemanager_path,
		.path_entry = filemanager_path_entry };
	const char *path = argc == 2 ? argv[1] : "/";
	const char *setup_step = "connect";
	int status = 1;

	if ((argc != 1 && argc != 2) || filemanager_validate_path(path) < 0 ||
	    filemanager_directory_load(&filemanager_directory_model, path) < 0) {
		fprintf(stderr, "filemanager: directory load failed: %d\n", errno);
		goto out;
	}
	strcpy(state.path, path);
	state.fd = filemanager_connect();
	if (state.fd < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "create window";
	if (filemanager_create_window(&state) < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "allocate surface";
	if (filemanager_resize_surface(&state, state.width, state.height) < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "load font";
	state.font = gui_font_load(FILEMANAGER_FONT_PATH, FILEMANAGER_FONT_HEIGHT);
	if (state.font == NULL) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	setup_step = "submit surface";
	if (filemanager_send_current_surface(&state) < 0) {
		fprintf(stderr, "filemanager: %s failed: %d\n", setup_step, errno);
		goto out;
	}
	printf("filemanager: window ready: %s (%zu entries%s)\n", path,
		filemanager_directory_model.count,
		filemanager_directory_model.omitted_count == 0 ? "" : ", truncated");
	fflush(stdout);
	status = 0;
	while (!state.close_requested) {
		gui_protocol_header header;
		int result;

		if (read_message(state.fd, &header, filemanager_input,
			 sizeof(filemanager_input)) < 0 ||
		    (result = filemanager_handle_message(&state, &header, filemanager_input,
			header.length - GUI_PROTOCOL_HEADER_SIZE)) < 0)
			break;
		if (result > 0 && filemanager_send_current_surface(&state) < 0)
			break;
	}
	if (state.close_requested) {
		uint8_t window_id[sizeof(gui_protocol_wire_window_id)];

		put32(window_id, state.window_id);
		(void)send_message(state.fd, GUI_PROTOCOL_WINDOW_DESTROY, window_id,
			sizeof(window_id));
	}
out:
	gui_font_destroy(state.font);
	gui_surface_destroy(state.surface);
	if (state.fd >= 0)
		close(state.fd);
	return status;
}
