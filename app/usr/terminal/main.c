#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <gui/font.h>
#include <gui/input.h>
#include <gui/protocol.h>
#include <gui/surface.h>
#include <app/gui-limits.h>

#include "vt.h"
#include <gui/surface_upload.h>
#include "config.h"

#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define TERMINAL_WIDTH 320U
#define TERMINAL_HEIGHT 192U
#define TERMINAL_CELL_WIDTH 8U
#define TERMINAL_CELL_HEIGHT 16U
#define TERMINAL_FONT_HEIGHT 14
#define TERMINAL_TITLE "Terminal"
#define TERMINAL_REPLY_BYTES 4096U
#define TERMINAL_PTY_INPUT_BYTES 4096U
#define TERMINAL_PTY_READ_QUANTUM 16U
#define TERMINAL_BACKGROUND 0x00111827U
#define TERMINAL_CURSOR 0x00f8fafcU
#define TERMINAL_CURSOR_BLINK_MS 500U
#define TERMINAL_SCROLLBACK_LINES 128U
#define TERMINAL_WHEEL_LINES 3
#define TERMINAL_FONT_PATH "/usr/share/fonts/DejaVuSansMono.ttf"

typedef struct {
	int fd;
	int master;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	int close_requested;
	app_gui_surface_flow surface_flow;
	terminal_vt vt;
	terminal_vt_cell *primary_cells;
	terminal_vt_cell *alternate_cells;
	terminal_vt_cell *scrollback_cells;
	terminal_vt_cell *rendered_cells;
	size_t rendered_cell_bytes;
	int rendered;
	uint32_t rendered_cursor_row;
	uint32_t rendered_cursor_column;
	int rendered_cursor_visible;
	terminal_vt_cursor_shape rendered_cursor_shape;
	uint64_t cursor_blink_deadline;
	int cursor_blink_on;
	gui_rect render_damage;
	uint64_t pending_configure_serial;
	uint32_t pending_width;
	uint32_t pending_height;
	gui_surface_upload upload;
	uint32_t capabilities;
	uint64_t base_frame;
	int render_pending;
	int destroy_queued;
	uint8_t pty_input[TERMINAL_PTY_INPUT_BYTES];
	size_t pty_input_bytes;
	gui_surface surface;
	gui_rect content;
	gui_color *pixels;
	gui_font *font;
	terminal_config config;
	uint32_t cell_width;
	uint32_t cell_height;
	int font_baseline;
} terminal_state;

static int terminal_resize_pty(int master, const terminal_state *state);

static uint8_t terminal_input[TERMINAL_REPLY_BYTES];

static uint64_t terminal_now_ms(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int terminal_cursor_visible(const terminal_state *state)
{
	return state->vt.view_offset == 0 && state->vt.cursor_visible && state->vt.cursor_row < state->vt.rows &&
		state->vt.cursor_column < state->vt.columns &&
		(!state->vt.cursor_blink || state->cursor_blink_deadline == 0 || state->cursor_blink_on);
}

static void terminal_reset_cursor_blink(terminal_state *state)
{
	state->cursor_blink_on = 1;
	state->cursor_blink_deadline = 0;
	state->render_pending = 1;
}

static void terminal_update_cursor_blink(terminal_state *state, uint64_t now)
{
	if (state->vt.view_offset != 0 || !state->vt.cursor_blink || !state->vt.cursor_visible ||
	    state->vt.cursor_row >= state->vt.rows || state->vt.cursor_column >= state->vt.columns) {
		state->cursor_blink_deadline = 0;
		state->cursor_blink_on = 1;
		return;
	}
	if (state->cursor_blink_deadline == 0) {
		state->cursor_blink_on = 1;
		state->cursor_blink_deadline = now + TERMINAL_CURSOR_BLINK_MS;
	} else if (now >= state->cursor_blink_deadline) {
		uint64_t periods = (now - state->cursor_blink_deadline) / TERMINAL_CURSOR_BLINK_MS + 1;

		state->cursor_blink_deadline += periods * TERMINAL_CURSOR_BLINK_MS;
		if (periods & 1) {
			state->cursor_blink_on = !state->cursor_blink_on;
			state->render_pending = 1;
		}
	}
}

static int terminal_cursor_timeout(const terminal_state *state, uint64_t now, int timeout)
{
	uint64_t remaining;
	int cursor_timeout;

	if (state->cursor_blink_deadline == 0)
		return timeout;
	remaining = state->cursor_blink_deadline > now ? state->cursor_blink_deadline - now : 0;
	cursor_timeout = remaining > INT_MAX ? INT_MAX : (int)remaining;
	return timeout < 0 || cursor_timeout < timeout ? cursor_timeout : timeout;
}

static const terminal_config *terminal_settings(const terminal_state *state)
{
	return state->config.font_size ? &state->config : &terminal_config_default;
}

static uint32_t terminal_cell_width(const terminal_state *state)
{
	return state->cell_width ? state->cell_width : TERMINAL_CELL_WIDTH;
}

static uint32_t terminal_cell_height(const terminal_state *state)
{
	return state->cell_height ? state->cell_height : TERMINAL_CELL_HEIGHT;
}

static int terminal_baseline(const terminal_state *state)
{
	return state->font_baseline ? state->font_baseline : TERMINAL_FONT_HEIGHT;
}

static int terminal_setup_font(terminal_state *state)
{
	gui_font_metrics metrics;
	gui_font_glyph glyph;

	state->font = gui_font_load(TERMINAL_FONT_PATH, terminal_settings(state)->font_size);
	if (state->font == NULL)
		return -1;
	if (terminal_settings(state)->font_size == TERMINAL_FONT_HEIGHT)
		return 0;
	if (!gui_font_get_metrics(state->font, &metrics) ||
	    !gui_font_get_glyph(state->font, 'M', &glyph) || glyph.advance <= 0 ||
	    metrics.ascent <= 0 || metrics.descent > 0 || metrics.line_gap < 0)
		return -1;
	state->cell_width = (uint32_t)glyph.advance;
	state->cell_height = (uint32_t)(metrics.ascent - metrics.descent + metrics.line_gap);
	state->font_baseline = metrics.ascent;
	return 0;
}

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
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

static uint16_t get16(const uint8_t *bytes)
{
	return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
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

static gui_color terminal_color(const terminal_state *state, uint32_t color, gui_color default_color)
{
	if ((color & ~TERMINAL_VT_COLOR_RGB_MASK) == TERMINAL_VT_COLOR_RGB_TAG)
		return color & TERMINAL_VT_COLOR_RGB_MASK;
	return color < 16 ? terminal_settings(state)->palette[color] : default_color;
}

static size_t terminal_encode_utf8(uint32_t codepoint, char bytes[4])
{
	if (codepoint < 0x80) {
		bytes[0] = codepoint;
		return 1;
	}
	if (codepoint < 0x800) {
		bytes[0] = 0xc0 | codepoint >> 6;
		bytes[1] = 0x80 | codepoint & 0x3f;
		return 2;
	}
	if (codepoint < 0x10000) {
		bytes[0] = 0xe0 | codepoint >> 12;
		bytes[1] = 0x80 | codepoint >> 6 & 0x3f;
		bytes[2] = 0x80 | codepoint & 0x3f;
		return 3;
	}
	bytes[0] = 0xf0 | codepoint >> 18;
	bytes[1] = 0x80 | codepoint >> 12 & 0x3f;
	bytes[2] = 0x80 | codepoint >> 6 & 0x3f;
	bytes[3] = 0x80 | codepoint & 0x3f;
	return 4;
}

static uint32_t terminal_axis_padding(uint32_t size, uint32_t cell_size, uint32_t padding)
{
	uint32_t limit = size > cell_size ? (size - cell_size) / 2 : 0;

	return padding < limit ? padding : limit;
}

static int terminal_resize_grid(terminal_state *state, uint32_t width,
				uint32_t height)
{
	terminal_vt_cell *primary_cells;
	terminal_vt_cell *alternate_cells;
	terminal_vt_cell *scrollback_cells;
	terminal_vt_cell *rendered_cells;
	gui_color *pixels;
	gui_rect content;
	uint32_t cell_width = terminal_cell_width(state);
	uint32_t cell_height = terminal_cell_height(state);
	uint32_t padding = terminal_settings(state)->padding;
	uint32_t rows;
	uint32_t columns;
	size_t cell_count;
	size_t scrollback_cell_count;
	size_t pixel_count;
	terminal_vt resized;

	pixels = NULL;
	scrollback_cells = NULL;

	if (width == 0 || height == 0 || width > INT_MAX || height > INT_MAX || width > UINT32_MAX / height ||
	    width > SIZE_MAX / height)
		return -1;
	content.x = terminal_axis_padding(width, cell_width, padding);
	content.y = terminal_axis_padding(height, cell_height, padding);
	content.w = width - 2 * (uint32_t)content.x;
	content.h = height - 2 * (uint32_t)content.y;
	columns = (uint32_t)content.w >= cell_width ? content.w / cell_width : 1;
	rows = (uint32_t)content.h >= cell_height ? content.h / cell_height : 1;
	if (rows > SIZE_MAX / columns / sizeof(*primary_cells))
		return -1;
	cell_count = (size_t)rows * columns;
	if (TERMINAL_SCROLLBACK_LINES > SIZE_MAX / columns)
		return -1;
	scrollback_cell_count = (size_t)TERMINAL_SCROLLBACK_LINES * columns;
	pixel_count = (size_t)width * height;
	if (pixel_count > SIZE_MAX / sizeof(*pixels))
		return -1;
	primary_cells = calloc(cell_count, sizeof(*primary_cells));
	alternate_cells = calloc(cell_count, sizeof(*alternate_cells));
	scrollback_cells = calloc(scrollback_cell_count, sizeof(*scrollback_cells));
	rendered_cells = calloc(cell_count, sizeof(*rendered_cells));
	pixels = malloc(pixel_count * sizeof(*pixels));
	if (primary_cells == NULL || alternate_cells == NULL || scrollback_cells == NULL ||
	    rendered_cells == NULL || pixels == NULL)
		goto out;
	if (state->primary_cells == NULL) {
		if (terminal_vt_init(&resized, primary_cells, alternate_cells, rows,
			columns) < 0)
			goto out;
		if (terminal_vt_set_scrollback(&resized, scrollback_cells,
			TERMINAL_SCROLLBACK_LINES) < 0)
			goto out;
	} else {
		resized = state->vt;
		if (terminal_vt_resize(&resized, primary_cells, alternate_cells,
			scrollback_cells, TERMINAL_SCROLLBACK_LINES, rows, columns) < 0)
			goto out;
	}
	free(state->primary_cells);
	free(state->alternate_cells);
	free(state->scrollback_cells);
	free(state->pixels);
	free(state->rendered_cells);
	state->primary_cells = primary_cells;
	state->alternate_cells = alternate_cells;
	state->scrollback_cells = scrollback_cells;
	state->rendered_cells = rendered_cells;
	state->rendered_cell_bytes = cell_count * sizeof(*rendered_cells);
	state->rendered = 0;
	state->render_damage = (gui_rect) { 0 };
	state->pixels = pixels;
	state->vt = resized;
	state->width = width;
	state->height = height;
	state->content = content;
	state->surface = (gui_surface) {
		.width = width,
		.height = height,
		.stride = width,
		.pixels = pixels,
	};
	return 0;
out:
	free(primary_cells);
	free(alternate_cells);
	free(scrollback_cells);
	free(rendered_cells);
	free(pixels);
	return -1;
}

static gui_rect terminal_cell_bounds(const terminal_state *state, uint32_t row, uint32_t column)
{
	return (gui_rect) { state->content.x + column * terminal_cell_width(state),
		state->content.y + row * terminal_cell_height(state),
		terminal_cell_width(state), terminal_cell_height(state) };
}

static gui_rect terminal_glyph_bounds(terminal_state *state, uint32_t row,
				     uint32_t column, uint32_t codepoint)
{
	gui_rect bounds = terminal_cell_bounds(state, row, column);
	gui_font_glyph glyph;
	gui_text_glyph decoded;
	char bytes[5] = { 0 };
	const char *cursor = bytes;

	if (codepoint == 0)
		return (gui_rect) { 0 };
	terminal_encode_utf8(codepoint, bytes);
	if (gui_text_next(&cursor, &decoded) && decoded.source == GUI_TEXT_GLYPH_FONT &&
	    gui_font_get_glyph(state->font, decoded.codepoint, &glyph)) {
		bounds.x += glyph.x_offset;
		bounds.y += terminal_baseline(state) - glyph.y_offset;
		bounds.w = glyph.width;
		bounds.h = glyph.height;
	} else {
		bounds.y += terminal_baseline(state) - 14;
		bounds.w = 8;
		bounds.h = 16;
	}
	return bounds;
}

static int terminal_rect_intersects(gui_rect first, gui_rect second)
{
	return first.w > 0 && first.h > 0 && second.w > 0 && second.h > 0 &&
		(int64_t)first.x < (int64_t)second.x + second.w &&
		(int64_t)second.x < (int64_t)first.x + first.w &&
		(int64_t)first.y < (int64_t)second.y + second.h &&
		(int64_t)second.y < (int64_t)first.y + first.h;
}

static void terminal_add_damage(gui_rect content, gui_rect *damage,
				gui_rect bounds)
{
	int64_t left = bounds.x < content.x ? content.x : bounds.x;
	int64_t top = bounds.y < content.y ? content.y : bounds.y;
	int64_t right = (int64_t)bounds.x + bounds.w;
	int64_t bottom = (int64_t)bounds.y + bounds.h;

	if (right > content.x + content.w)
		right = content.x + content.w;
	if (bottom > content.y + content.h)
		bottom = content.y + content.h;
	if (bounds.w <= 0 || bounds.h <= 0 || left >= right || top >= bottom)
		return;
	if (damage->w > 0 && damage->h > 0) {
		if (left > damage->x)
			left = damage->x;
		if (top > damage->y)
			top = damage->y;
		if (right < (int64_t)damage->x + damage->w)
			right = (int64_t)damage->x + damage->w;
		if (bottom < (int64_t)damage->y + damage->h)
			bottom = (int64_t)damage->y + damage->h;
	}
	*damage = (gui_rect) { left, top, right - left, bottom - top };
}

static int terminal_same_cell(const terminal_vt_cell *first, const terminal_vt_cell *second)
{
	return first->codepoint == second->codepoint && first->foreground == second->foreground &&
		first->background == second->background && first->attributes == second->attributes;
}

static void terminal_cell_colors(const terminal_state *state, const terminal_vt_cell *cell,
				 gui_color *foreground, gui_color *background)
{
	*foreground = terminal_color(state, cell->foreground, terminal_settings(state)->foreground);
	*background = terminal_color(state, cell->background, terminal_settings(state)->background);
	if (cell->attributes & TERMINAL_VT_ATTRIBUTE_INVERSE) {
		gui_color color = *foreground;

		*foreground = *background;
		*background = color;
	}
}

static void terminal_draw_cell_background(const terminal_state *state,
					  gui_surface *surface, uint32_t row,
					  uint32_t column, const terminal_vt_cell *cell,
					  int offset_x, int offset_y)
{
	gui_rect bounds = terminal_cell_bounds(state, row, column);
	gui_color foreground;
	gui_color background;

	bounds.x -= offset_x;
	bounds.y -= offset_y;
	terminal_cell_colors(state, cell, &foreground, &background);
	(void)foreground;
	gui_fill_rect(surface, bounds, background);
}

static void terminal_draw_cell_foreground(const terminal_state *state,
					  gui_surface *surface, uint32_t row,
					  uint32_t column, const terminal_vt_cell *cell,
					  int offset_x, int offset_y)
{
	gui_rect bounds = terminal_cell_bounds(state, row, column);
	gui_color foreground;
	gui_color background;

	bounds.x -= offset_x;
	bounds.y -= offset_y;
	terminal_cell_colors(state, cell, &foreground, &background);
	(void)background;
	if (cell->codepoint != 0) {
		char bytes[5] = { 0 };

		terminal_encode_utf8(cell->codepoint, bytes);
		gui_font_draw_text(surface, bounds.x, bounds.y + terminal_baseline(state),
			state->font, bytes, foreground);
	}
	if (cell->attributes & TERMINAL_VT_ATTRIBUTE_UNDERLINE)
		gui_fill_rect(surface, (gui_rect) { bounds.x, bounds.y + (int)terminal_cell_height(state) - 2,
			terminal_cell_width(state), 1 }, foreground);
}

static void terminal_draw_cursor(const terminal_state *state, gui_surface *surface,
				 int offset_x, int offset_y)
{
	gui_rect bounds = terminal_cell_bounds(state, state->vt.cursor_row, state->vt.cursor_column);
	const terminal_vt_cell *cell = state->vt.cells +
		(size_t)state->vt.cursor_row * state->vt.columns + state->vt.cursor_column;
	gui_surface clipped;
	int left;
	int top;
	int right;
	int bottom;

	bounds.x -= offset_x;
	bounds.y -= offset_y;
	if (state->vt.cursor_shape == TERMINAL_VT_CURSOR_UNDERLINE) {
		bounds.y += terminal_cell_height(state) - 2;
		bounds.h = 2;
	} else if (state->vt.cursor_shape == TERMINAL_VT_CURSOR_BAR)
		bounds.w = 2;
	left = bounds.x < 0 ? 0 : bounds.x;
	top = bounds.y < 0 ? 0 : bounds.y;
	right = (int64_t)bounds.x + bounds.w > surface->width ? surface->width : bounds.x + bounds.w;
	bottom = (int64_t)bounds.y + bounds.h > surface->height ? surface->height : bounds.y + bounds.h;
	if (right <= left || bottom <= top)
		return;
	gui_fill_rect(surface, bounds, terminal_settings(state)->cursor);
	if (state->vt.cursor_shape != TERMINAL_VT_CURSOR_BLOCK)
		return;
	clipped = (gui_surface) { .width = right - left, .height = bottom - top,
		.stride = surface->stride, .pixels = surface->pixels +
			(size_t)top * surface->stride + left };
	if (cell->codepoint != 0) {
		char bytes[5] = { 0 };

		terminal_encode_utf8(cell->codepoint, bytes);
		gui_font_draw_text(&clipped, bounds.x - left, bounds.y - top + terminal_baseline(state),
			state->font, bytes, terminal_settings(state)->background);
	}
	if (cell->attributes & TERMINAL_VT_ATTRIBUTE_UNDERLINE)
		gui_fill_rect(&clipped, (gui_rect) { bounds.x - left,
			bounds.y - top + (int)terminal_cell_height(state) - 2, terminal_cell_width(state), 1 },
			terminal_settings(state)->background);
}

static int terminal_render(terminal_state *state)
{
	gui_rect damage = { 0 };
	gui_surface clipped;
	gui_color background = terminal_settings(state)->background;
	int cursor_visible = terminal_cursor_visible(state);

	if (state->font == NULL || state->surface.pixels == NULL || state->rendered_cells == NULL)
		return -1;
	if (!state->rendered) {
		damage = state->content;
	} else {
		for (uint32_t row = 0; row < state->vt.rows; row++) {
			const terminal_vt_cell *line = terminal_vt_view_line(&state->vt, row);

			for (uint32_t column = 0; column < state->vt.columns; column++) {
				size_t index = (size_t)row * state->vt.columns + column;
				const terminal_vt_cell *cell = line + column;
				const terminal_vt_cell *old = state->rendered_cells + index;

				if (terminal_same_cell(cell, old))
					continue;
				terminal_add_damage(state->content, &damage, terminal_cell_bounds(state, row, column));
				terminal_add_damage(state->content, &damage, terminal_glyph_bounds(state, row, column, old->codepoint));
				terminal_add_damage(state->content, &damage, terminal_glyph_bounds(state, row, column, cell->codepoint));
			}
		}
		if (cursor_visible != state->rendered_cursor_visible ||
		    state->vt.cursor_shape != state->rendered_cursor_shape ||
		    state->vt.cursor_row != state->rendered_cursor_row ||
		    state->vt.cursor_column != state->rendered_cursor_column) {
			if (state->rendered_cursor_visible)
				terminal_add_damage(state->content, &damage, terminal_cell_bounds(state,
					state->rendered_cursor_row, state->rendered_cursor_column));
			if (cursor_visible)
				terminal_add_damage(state->content, &damage, terminal_cell_bounds(state, state->vt.cursor_row, state->vt.cursor_column));
		}
	}
	state->render_damage = damage;
	if (damage.w == 0 || damage.h == 0)
		return 0;
	clipped = (gui_surface) { .width = damage.w, .height = damage.h,
		.stride = state->surface.stride, .pixels = state->pixels +
			(size_t)damage.y * state->surface.stride + damage.x };
	if (!state->rendered) {
		state->render_damage = (gui_rect) { 0, 0, state->width, state->height };
		gui_fill_rect(&state->surface, state->render_damage, background);
	} else {
		gui_fill_rect(&clipped, (gui_rect) { 0, 0, damage.w, damage.h }, background);
	}
	for (uint32_t row = 0; row < state->vt.rows; row++) {
		const terminal_vt_cell *line = terminal_vt_view_line(&state->vt, row);

		for (uint32_t column = 0; column < state->vt.columns; column++) {
			const terminal_vt_cell *cell = line + column;

			if (terminal_rect_intersects(damage, terminal_cell_bounds(state, row, column)) ||
			    terminal_rect_intersects(damage, terminal_glyph_bounds(state, row, column, cell->codepoint)))
				terminal_draw_cell_background(state, &clipped, row, column, cell,
					damage.x, damage.y);
		}
	}
	for (uint32_t row = 0; row < state->vt.rows; row++) {
		const terminal_vt_cell *line = terminal_vt_view_line(&state->vt, row);

		for (uint32_t column = 0; column < state->vt.columns; column++) {
			const terminal_vt_cell *cell = line + column;

			if (terminal_rect_intersects(damage, terminal_cell_bounds(state, row, column)) ||
			    terminal_rect_intersects(damage, terminal_glyph_bounds(state, row, column, cell->codepoint)))
				terminal_draw_cell_foreground(state, &clipped, row, column, cell,
					damage.x, damage.y);
		}
		if (state->vt.view_offset != 0)
			memcpy(state->rendered_cells + (size_t)row * state->vt.columns, line,
				(size_t)state->vt.columns * sizeof(*line));
	}
	if (cursor_visible)
		terminal_draw_cursor(state, &clipped, damage.x, damage.y);
	if (state->vt.view_offset == 0)
		memcpy(state->rendered_cells, state->vt.cells, state->rendered_cell_bytes);
	state->rendered_cursor_row = state->vt.cursor_row;
	state->rendered_cursor_column = state->vt.cursor_column;
	state->rendered_cursor_visible = cursor_visible;
	state->rendered_cursor_shape = state->vt.cursor_shape;
	state->rendered = 1;
	return 0;
}

static int terminal_enqueue_input(terminal_state *state, const void *bytes, size_t count)
{
	if (count > sizeof(state->pty_input) - state->pty_input_bytes)
		return -1;
	memcpy(state->pty_input + state->pty_input_bytes, bytes, count);
	state->pty_input_bytes += count;
	return 0;
}

static int terminal_enqueue_vt_response(terminal_state *state)
{
	if (state->master < 0)
		return 0;
	while (state->vt.response_bytes != 0 &&
	       state->pty_input_bytes < sizeof(state->pty_input)) {
		size_t available = sizeof(state->pty_input) - state->pty_input_bytes;
		size_t count = terminal_vt_take_response(&state->vt,
			state->pty_input + state->pty_input_bytes, available);

		if (count == 0)
			break;
		state->pty_input_bytes += count;
	}
	return 0;
}

static int terminal_flush_input(terminal_state *state)
{
	ssize_t count;

	if (state->pty_input_bytes == 0)
		return 0;
	count = write(state->master, state->pty_input, state->pty_input_bytes);
	if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
		return 0;
	if (count <= 0 || (size_t)count > state->pty_input_bytes)
		return -1;
	state->pty_input_bytes -= count;
	memmove(state->pty_input, state->pty_input + count, state->pty_input_bytes);
	return 0;
}

static int terminal_write_input(terminal_state *state, uint32_t keysym,
				uint32_t modifiers)
{
	static const char cursor_sequences[][3] = { "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D" };
	uint8_t byte;
	const char *sequence = NULL;

	if (state->master < 0)
		return 0;
	switch (keysym) {
	case GUI_KEYSYM_UP: sequence = cursor_sequences[0]; break;
	case GUI_KEYSYM_DOWN: sequence = cursor_sequences[1]; break;
	case GUI_KEYSYM_RIGHT: sequence = cursor_sequences[2]; break;
	case GUI_KEYSYM_LEFT: sequence = cursor_sequences[3]; break;
	default: break;
	}
	if (sequence != NULL) {
		char encoded[3] = { '\x1b', state->vt.application_cursor ? 'O' : '[', sequence[2] };

		return terminal_enqueue_input(state, encoded, sizeof(encoded));
	}
	if (keysym == GUI_KEYSYM_ESCAPE)
		byte = '\x1b';
	else if (keysym == GUI_KEYSYM_BACKSPACE)
		byte = '\x7f';
	else if (keysym == '\n')
		byte = '\r';
	else if (keysym <= 0x7f) {
		byte = keysym;
		if (modifiers & GUI_MOD_CTRL)
			byte &= 0x1f;
	} else
		return 0;
	return terminal_enqueue_input(state, &byte, sizeof(byte));
}

static void terminal_write_wheel(terminal_state *state, int32_t x, int32_t y,
				 uint32_t modifiers, int wheel)
{
	unsigned int button = wheel < 0 ? 64 : 65;
	unsigned int column;
	unsigned int row;
	char report[32];
	int length;

	if (state->vt.mouse_tracking == 0 || (modifiers & GUI_MOD_SHIFT)) {
		if (terminal_vt_scroll_view(&state->vt, -wheel * TERMINAL_WHEEL_LINES))
			terminal_reset_cursor_blink(state);
		return;
	}
	if (state->master < 0 || x < state->content.x || y < state->content.y ||
	    x - state->content.x >= state->content.w || y - state->content.y >= state->content.h)
		return;
	column = (x - state->content.x) / terminal_cell_width(state) + 1;
	row = (y - state->content.y) / terminal_cell_height(state) + 1;
	if (column > state->vt.columns || row > state->vt.rows)
		return;
	if (modifiers & GUI_MOD_ALT)
		button |= 8;
	if (modifiers & GUI_MOD_CTRL)
		button |= 16;
	if (state->vt.mouse_sgr) {
		length = snprintf(report, sizeof(report), "\x1b[<%u;%u;%uM", button, column, row);
	} else {
		if (column > 223 || row > 223)
			return;
		memcpy(report, "\x1b[M", 3);
		report[3] = 32 + button;
		report[4] = 32 + column;
		report[5] = 32 + row;
		length = 6;
	}
	if (length <= 0 || (size_t)length >= sizeof(report))
		return;
	state->vt.view_offset = 0;
	terminal_reset_cursor_blink(state);
	for (int steps = abs(wheel); steps > 0; steps--)
		if (terminal_enqueue_input(state, report, length) < 0)
			break;
}

static int terminal_handle_message(terminal_state *state,
				   const gui_protocol_header *header,
				   const uint8_t *payload, size_t payload_bytes)
{
	int flow_result = app_gui_surface_flow_message(&state->surface_flow,
		state->window_id, header->type, payload, payload_bytes);

	if (flow_result != 0) {
		if (flow_result > 0 && header->type == GUI_PROTOCOL_ERROR)
			fprintf(stderr, "terminal: deferred surface error %u: %d\n",
				get16(payload + 4), (int32_t)get32(payload));
		return flow_result < 0 ? -1 : 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != state->window_id ||
		    get64(payload + 4) <= state->configure_serial ||
		    get64(payload + 4) <= state->pending_configure_serial ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0)
			return -1;
		if (terminal_resize_grid(state, get32(payload + 12), get32(payload + 16)) < 0) {
			state->pending_configure_serial = get64(payload + 4);
			state->pending_width = get32(payload + 12);
			state->pending_height = get32(payload + 16);
			app_gui_surface_defer(&state->surface_flow);
			fprintf(stderr, "terminal: grid resize deferred\n");
			return 1;
		}
		state->configure_serial = get64(payload + 4);
		state->pending_configure_serial = 0;
		app_gui_surface_defer(&state->surface_flow);
		if (state->master >= 0 && terminal_resize_pty(state->master, state) < 0)
			return -1;
		return 1;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_id) ||
		    get32(payload) != state->window_id)
			return -1;
		state->close_requested = 1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_STATE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_state) ||
		    get32(payload) != state->window_id ||
		    get32(payload + 4) > GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED)
			return -1;
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
		if (get32(payload + 4) == GUI_INPUT_EVENT_KEY && get32(payload + 8) != 0) {
			if (terminal_write_input(state, get32(payload + 8), get32(payload + 12)) < 0)
				return -1;
			state->vt.view_offset = 0;
			terminal_reset_cursor_blink(state);
		} else if (get32(payload + 4) == GUI_INPUT_EVENT_MOUSE) {
			int8_t wheel = payload[offsetof(gui_protocol_wire_input_event, wheel)];

			if (wheel != 0)
				terminal_write_wheel(state, (int32_t)get32(payload + 16),
					(int32_t)get32(payload + 20), get32(payload + 12), wheel);
		}
		return 0;
	}
	return -1;
}

static int terminal_send_surface(terminal_state *state)
{
	gui_rect rectangle;
	int patch;

	if (!gui_surface_upload_ready(&state->upload) || !state->render_pending)
		return 0;
	if (terminal_render(state) < 0)
		return -1;
	state->render_pending = 0;
	if (state->base_frame != 0 && state->render_damage.w == 0)
		return 0;
	if (state->frame_serial == UINT64_MAX)
		return -1;
	patch = (state->capabilities & GUI_PROTOCOL_CAP_SURFACE_PATCH) && state->base_frame != 0;
	rectangle = patch ? state->render_damage : (gui_rect) { 0, 0, state->width, state->height };
	return gui_surface_upload_start(&state->upload, state->window_id, state->configure_serial,
		state->base_frame, ++state->frame_serial, &state->surface, rectangle, patch);
}

static int terminal_connect(void)
{
	struct sockaddr_un address = { 0 };
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
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

static int terminal_negotiate(terminal_state *state, uint16_t minor)
{
	uint8_t frame[32];
	uint8_t payload[sizeof(gui_protocol_wire_hello)];
	gui_protocol_header header;

	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR, minor }) < 0 ||
	    write_full(state->fd, frame, GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello)) < 0 ||
	    read_message(state->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello) ||
	    get16(payload) != GUI_PROTOCOL_VERSION_MAJOR || get16(payload + 2) > minor)
		return -1;
	if (get16(payload + 2) == 0)
		return 0;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_CAPABILITIES, sizeof(payload)) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, GUI_PROTOCOL_CAP_SURFACE_PATCH | GUI_PROTOCOL_CAP_WINDOW_OPACITY);
	if (write_full(state->fd, frame, GUI_PROTOCOL_HEADER_SIZE + sizeof(payload)) < 0 ||
	    read_message(state->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_CAPABILITIES_REPLY || header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(payload) ||
	    (get32(payload) & ~(GUI_PROTOCOL_CAP_SURFACE_PATCH | GUI_PROTOCOL_CAP_WINDOW_OPACITY)) != 0)
		return -1;
	state->capabilities = get32(payload);
	return 0;
}

static int terminal_create_window(terminal_state *state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		sizeof(TERMINAL_TITLE) - 1] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_create_reply)];
	gui_protocol_header header;
	size_t title_bytes = sizeof(TERMINAL_TITLE) - 1;

	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
			sizeof(gui_protocol_wire_window_create) + title_bytes) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 4, TERMINAL_WIDTH);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 8, TERMINAL_HEIGHT);
	put16(frame + GUI_PROTOCOL_HEADER_SIZE + 12, title_bytes);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		TERMINAL_TITLE, title_bytes);
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

static int terminal_set_opacity(terminal_state *state)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_opacity)];
	unsigned int opacity = terminal_settings(state)->opacity;

	if (opacity == 100)
		return 0;
	if (!(state->capabilities & GUI_PROTOCOL_CAP_WINDOW_OPACITY)) {
		fprintf(stderr, "terminal: window opacity unsupported; using opaque content\n");
		return 0;
	}
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_OPACITY,
		sizeof(gui_protocol_wire_window_opacity)) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, state->window_id);
	put32(frame + GUI_PROTOCOL_HEADER_SIZE + 4, opacity);
	return write_full(state->fd, frame, sizeof(frame));
}

static int terminal_winsize(const terminal_state *state, struct winsize *size)
{
	if (state->vt.rows > UINT16_MAX || state->vt.columns > UINT16_MAX ||
	    state->content.w > UINT16_MAX || state->content.h > UINT16_MAX)
		return -1;
	*size = (struct winsize) {
		.ws_row = state->vt.rows,
		.ws_col = state->vt.columns,
		.ws_xpixel = state->content.w,
		.ws_ypixel = state->content.h,
	};
	return 0;
}

static int terminal_resize_pty(int master, const terminal_state *state)
{
	struct winsize size;

	if (terminal_winsize(state, &size) < 0)
		return -1;
	/* TIOCSWINSZ notifies the PTY foreground process group with SIGWINCH. */
	return ioctl(master, TIOCSWINSZ, &size);
}

static int terminal_apply_pending_configure(terminal_state *state, int master)
{
	if (state->surface_flow.retry_pending)
		return 0;
	if (state->pending_configure_serial != 0) {
		if (terminal_resize_grid(state, state->pending_width, state->pending_height) < 0) {
			app_gui_surface_defer(&state->surface_flow);
			return 0;
		}
		state->configure_serial = state->pending_configure_serial;
		state->pending_configure_serial = 0;
		if (master >= 0 && terminal_resize_pty(master, state) < 0)
			return -1;
	}
	return 0;
}

static int terminal_send_current_surface(terminal_state *state, int master)
{
	if (terminal_apply_pending_configure(state, master) < 0)
		return -1;
	if (state->surface_flow.retry_pending)
		return 0;
	return terminal_send_surface(state);
}

static int terminal_send_title(terminal_state *state)
{
	uint8_t message[sizeof(gui_protocol_wire_window_set_title) +
		GUI_PROTOCOL_MAX_TITLE_BYTES] = { 0 };
	const uint8_t *title;
	uint16_t title_bytes;
	size_t message_bytes;

	if (!terminal_vt_take_title(&state->vt, &title, &title_bytes))
		return 0;
	if (title_bytes > GUI_PROTOCOL_MAX_TITLE_BYTES)
		return -1;
	put32(message, state->window_id);
	put16(message + 4, title_bytes);
	memcpy(message + sizeof(gui_protocol_wire_window_set_title), title, title_bytes);
	message_bytes = sizeof(gui_protocol_wire_window_set_title) + title_bytes;
	return gui_surface_upload_control(&state->upload, GUI_PROTOCOL_WINDOW_SET_TITLE, message, message_bytes);
}

static int terminal_spawn_pty(const char *command, const terminal_state *state,
			      int *master, pid_t *child)
{
	struct winsize size;
	pid_t pid;
	int flags;

	if (terminal_winsize(state, &size) < 0)
		return -1;
	pid = forkpty(master, NULL, NULL, &size);
	if (pid < 0)
		return -1;
	if (pid == 0) {
		if ((state->fd > STDERR_FILENO && close(state->fd) < 0) ||
		    signal(SIGHUP, SIG_DFL) == SIG_ERR || signal(SIGPIPE, SIG_DFL) == SIG_ERR)
			_exit(127);
		setenv("PATH", "/bin:/usr/bin:/bin/toybox", 1);
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		if (command != NULL)
			execl("/bin/oksh", "oksh", "-c", command, NULL);
		else
			execl("/bin/oksh", "oksh", NULL);
		_exit(127);
	}
	flags = fcntl(*master, F_GETFL);
	if (flags < 0 || fcntl(*master, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(*master);
		return -1;
	}
	*child = pid;
	return 0;
}

static int terminal_finish_child(int master, pid_t child)
{
	int status;

	if (close(master) < 0 || waitpid(child, &status, 0) != child)
		return -1;
	if (!WIFEXITED(status)) {
		fprintf(stderr, "terminal: child did not exit normally\n");
		return -1;
	}
	printf("terminal: child exit %d\n", WEXITSTATUS(status));
	fflush(stdout);
	return WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int terminal_drain_pty(int master, terminal_vt *vt, int *updated)
{
	uint8_t buffer[512];

	for (unsigned int batch = 0; batch < TERMINAL_PTY_READ_QUANTUM; batch++) {
		ssize_t count = read(master, buffer, sizeof(buffer));

		if (count > 0) {
			terminal_vt_feed(vt, buffer, count);
			*updated = 1;
			if (write_full(STDOUT_FILENO, buffer, count) < 0)
				return -1;
			continue;
		}
		if (count == 0 || (count < 0 && errno == EIO))
			return 1;
		if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return 0;
		if (count < 0 && errno == EINTR)
			continue;
		return -1;
	}
	return 0;
}

static ssize_t terminal_socket_write(void *context, const void *bytes, size_t count)
{
	return write(*(const int *)context, bytes, count);
}

static int terminal_process_message(terminal_state *state, const gui_protocol_header *header,
				    const uint8_t *payload, size_t bytes)
{
	if (header->type == GUI_PROTOCOL_SURFACE_DATA_REPLY)
		return gui_surface_upload_reply(&state->upload, payload, bytes);
	if (header->type == GUI_PROTOCOL_ERROR && bytes == sizeof(gui_protocol_wire_error)) {
		gui_protocol_type failed = get16(payload + 4);
		int32_t status = (int32_t)get32(payload);

		if (failed == GUI_PROTOCOL_SURFACE_CANCEL)
			return 0;
		if (failed == GUI_PROTOCOL_SURFACE_BEGIN || failed == GUI_PROTOCOL_SURFACE_PATCH_BEGIN ||
		    failed == GUI_PROTOCOL_SURFACE_DATA || failed == GUI_PROTOCOL_SURFACE_COMMIT) {
			int canceled = state->upload.canceled;

			if (failed == GUI_PROTOCOL_SURFACE_DATA && gui_surface_upload_data_error(&state->upload) < 0)
				return -1;
			if (status != -ENOMEM && status != -EBUSY && status != -EINVAL && status != -ESTALE && status != -EIO)
				return -1;
			if (!canceled || failed == GUI_PROTOCOL_SURFACE_COMMIT) {
				gui_surface_upload_cancel(&state->upload, state->capabilities & GUI_PROTOCOL_CAP_SURFACE_PATCH);
				state->base_frame = 0;
				state->render_pending = 1;
				app_gui_surface_defer(&state->surface_flow);
				fprintf(stderr, "terminal: deferred surface error %u: %d\n", failed, status);
			}
			return 0;
		}
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (bytes != sizeof(gui_protocol_wire_window_configure) || get32(payload) != state->window_id)
			return -1;
		gui_surface_upload_cancel(&state->upload, 0);
		state->base_frame = 0;
		state->render_pending = 1;
	}
	return terminal_handle_message(state, header, payload, bytes);
}

static int terminal_drain_replies(terminal_state *state, size_t *input_bytes)
{
	size_t consumed = 0;

	for (unsigned int messages = 0; messages < 64; messages++) {
		gui_protocol_header header;

		if (*input_bytes - consumed < GUI_PROTOCOL_HEADER_SIZE)
			break;
		if (gui_protocol_decode_header(terminal_input + consumed, *input_bytes - consumed, &header) < 0 ||
		    header.length > 64)
			return -1;
		if (header.length > *input_bytes - consumed)
			break;
		if (terminal_process_message(state, &header, terminal_input + consumed + GUI_PROTOCOL_HEADER_SIZE,
			header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)
			return -1;
		consumed += header.length;
	}
	*input_bytes -= consumed;
	memmove(terminal_input, terminal_input + consumed, *input_bytes);
	if (*input_bytes >= GUI_PROTOCOL_HEADER_SIZE) {
		gui_protocol_header header;

		if (gui_protocol_decode_header(terminal_input, *input_bytes, &header) < 0 || header.length > 64)
			return -1;
		return header.length <= *input_bytes;
	}
	return 0;
}

int main(int argc, char **argv)
{
	long descriptor_limit = sysconf(_SC_OPEN_MAX);
	static terminal_state state = { .fd = -1, .master = -1 };
	const char *command = NULL;
	pid_t child = -1;
	int master = -1;
	int status = 1;
	int pty_eof = 0;
	int closing = 0;
	size_t input_bytes = 0;

	if (descriptor_limit < 0 || signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		return 1;
	memset(terminal_input, 0, sizeof(terminal_input));
	for (long descriptor = STDERR_FILENO + 1; descriptor < descriptor_limit; descriptor++)
		close(descriptor);
	if (argc == 3 && !strcmp(argv[1], "--command"))
		command = argv[2];
	else if (argc != 1)
		return 1;
	terminal_config_load(&state.config);
	if (terminal_setup_font(&state) < 0) {
		fprintf(stderr, "terminal: font setup failed\n");
		goto out;
	}
	state.fd = terminal_connect();
	if (state.fd < 0) {
		fprintf(stderr, "terminal: connect failed: %d\n", errno);
		goto out;
	}
	if (terminal_negotiate(&state, GUI_PROTOCOL_VERSION_MINOR) < 0) {
		close(state.fd);
		state.fd = terminal_connect();
		if (state.fd < 0 || terminal_negotiate(&state, 0) < 0)
			goto out;
	}
	if (terminal_create_window(&state) < 0) {
		fprintf(stderr, "terminal: create failed\n");
		goto out;
	}
	if (terminal_set_opacity(&state) < 0)
		goto out;
	if (terminal_resize_grid(&state, state.width, state.height) < 0) {
		fprintf(stderr, "terminal: grid setup failed\n");
		goto out;
	}
	if (terminal_spawn_pty(command, &state, &master, &child) < 0) {
		fprintf(stderr, "terminal: PTY setup failed: %d\n", errno);
		goto out;
	}
	state.master = master;
	if (fcntl(state.fd, F_SETFL, O_NONBLOCK) < 0)
		goto out;
	state.render_pending = 1;
	puts("terminal: window ready");
	fflush(stdout);
	status = 0;
	for (;;) {
		struct pollfd pollfds[2];
		int result;
		int buffered = terminal_drain_replies(&state, &input_bytes);
		int timeout;

		if (buffered < 0)
			break;
		if (terminal_enqueue_vt_response(&state) < 0)
			break;
		if (state.upload.completed) {
			if (state.upload.completed_configure == state.configure_serial && !state.surface_flow.retry_pending)
				state.base_frame = state.upload.completed_serial;
			state.upload.completed = 0;
		}
		if (state.close_requested) {
			if (!closing) {
				gui_surface_upload_cancel(&state.upload, state.capabilities & GUI_PROTOCOL_CAP_SURFACE_PATCH);
				closing = 1;
			}
			if (!state.upload.active && state.upload.frame_bytes == 0) {
				uint8_t window_id[sizeof(gui_protocol_wire_window_id)];

				if (state.destroy_queued)
					break;
				put32(window_id, state.window_id);
				if (gui_surface_upload_control(&state.upload, GUI_PROTOCOL_WINDOW_DESTROY, window_id, sizeof(window_id)) < 0)
					break;
				state.destroy_queued = 1;
			}
		} else {
			if (!pty_eof)
				terminal_update_cursor_blink(&state, terminal_now_ms());
			if (app_gui_surface_retry_ready(&state.surface_flow))
				state.render_pending = 1;
			if (terminal_send_current_surface(&state, master) < 0)
				break;
			if (state.upload.frame_bytes == 0 && terminal_send_title(&state) < 0)
				break;
			if (pty_eof && gui_surface_upload_ready(&state.upload) && !state.render_pending && !state.surface_flow.retry_pending) {
				status = terminal_finish_child(master, child) == 0 ? 0 : 1;
				master = state.master = -1;
				state.close_requested = 1;
				continue;
			}
		}
		pollfds[0] = (struct pollfd) { .fd = state.fd, .events = POLLIN |
			(gui_surface_upload_writable(&state.upload) ? POLLOUT : 0) };
		pollfds[1] = (struct pollfd) { .fd = pty_eof || state.close_requested ? -1 : master,
			.events = POLLIN | (state.pty_input_bytes ? POLLOUT : 0) };
		timeout = buffered ? 0 : state.surface_flow.retry_pending ? APP_GUI_SURFACE_RETRY_MS : -1;
		if (!pty_eof && !state.close_requested)
			timeout = terminal_cursor_timeout(&state, terminal_now_ms(), timeout);
		result = poll(pollfds, 2, timeout);
		if (result < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (result == 0)
			continue;
		if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
			break;
		if (pollfds[0].revents & POLLIN) {
			ssize_t count = recv(state.fd, terminal_input + input_bytes, sizeof(terminal_input) - input_bytes, 0);

			if (count > 0)
				input_bytes += count;
			else if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
				break;
		}
		if ((pollfds[0].revents & POLLOUT) && gui_surface_upload_write(&state.upload, terminal_socket_write, &state.fd) < 0)
			break;
		if ((pollfds[1].revents & POLLOUT) && terminal_flush_input(&state) < 0)
			break;
		if (pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			int updated = 0;

			result = terminal_drain_pty(master, &state.vt, &updated);
			if (result < 0)
				break;
			if (updated)
				terminal_reset_cursor_blink(&state);
			if (result > 0)
				pty_eof = 1;
		}
	}
out:
	if (master >= 0)
		close(master);
	gui_font_destroy(state.font);
	free(state.primary_cells);
	free(state.alternate_cells);
	free(state.scrollback_cells);
	free(state.rendered_cells);
	free(state.pixels);
	if (state.fd >= 0)
		close(state.fd);
	return status;
}
