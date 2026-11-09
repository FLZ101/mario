#include "vt.h"

#include <string.h>

#define TERMINAL_VT_REPLACEMENT 0xfffdu

static void terminal_vt_scroll(terminal_vt *terminal)
{
	size_t row_bytes = (size_t)terminal->columns * sizeof(*terminal->cells);

	/* The grid has at least two rows when this moves a row upward. */
	if (terminal->rows > 1)
		memmove(terminal->cells, terminal->cells + terminal->columns,
			row_bytes * (terminal->rows - 1));
	memset(terminal->cells + (size_t)terminal->columns * (terminal->rows - 1), 0,
		row_bytes);
}

static void terminal_vt_line_feed(terminal_vt *terminal)
{
	if (terminal->cursor_row + 1 < terminal->rows) {
		terminal->cursor_row++;
		return;
	}
	terminal_vt_scroll(terminal);
}

static void terminal_vt_put(terminal_vt *terminal, uint32_t codepoint)
{
	terminal->cells[(size_t)terminal->cursor_row * terminal->columns +
		terminal->cursor_column].codepoint = codepoint;
	terminal->cursor_column++;
	if (terminal->cursor_column == terminal->columns) {
		terminal->cursor_column = 0;
		terminal_vt_line_feed(terminal);
	}
}

static void terminal_vt_control(terminal_vt *terminal, uint8_t byte)
{
	if (byte == '\r') {
		terminal->cursor_column = 0;
		return;
	}
	if (byte == '\n') {
		terminal_vt_line_feed(terminal);
		return;
	}
	if (byte == '\b' && terminal->cursor_column != 0)
		terminal->cursor_column--;
}

static void terminal_vt_reset_utf8(terminal_vt *terminal)
{
	terminal->utf8_codepoint = 0;
	terminal->utf8_minimum = 0;
	terminal->utf8_remaining = 0;
}

static void terminal_vt_feed_byte(terminal_vt *terminal, uint8_t byte)
{
again:
	if (terminal->utf8_remaining != 0) {
		if ((byte & 0xc0) == 0x80) {
			terminal->utf8_codepoint = terminal->utf8_codepoint << 6 |
				(byte & 0x3f);
			if (--terminal->utf8_remaining != 0)
				return;
			if (terminal->utf8_codepoint >= terminal->utf8_minimum &&
			    terminal->utf8_codepoint <= 0x10ffff &&
			    (terminal->utf8_codepoint < 0xd800 ||
			     terminal->utf8_codepoint > 0xdfff))
				terminal_vt_put(terminal, terminal->utf8_codepoint);
			else
				terminal_vt_put(terminal, TERMINAL_VT_REPLACEMENT);
			terminal_vt_reset_utf8(terminal);
			return;
		}
		/* Emit one replacement, then reprocess this byte as a new character. */
		terminal_vt_reset_utf8(terminal);
		terminal_vt_put(terminal, TERMINAL_VT_REPLACEMENT);
		goto again;
	}

	if (byte < 0x20 || byte == 0x7f) {
		terminal_vt_control(terminal, byte);
		return;
	}
	if (byte < 0x80) {
		terminal_vt_put(terminal, byte);
		return;
	}
	if (byte >= 0xc2 && byte <= 0xdf) {
		terminal->utf8_codepoint = byte & 0x1f;
		terminal->utf8_minimum = 0x80;
		terminal->utf8_remaining = 1;
		return;
	}
	if (byte >= 0xe0 && byte <= 0xef) {
		terminal->utf8_codepoint = byte & 0x0f;
		terminal->utf8_minimum = 0x800;
		terminal->utf8_remaining = 2;
		return;
	}
	if (byte >= 0xf0 && byte <= 0xf4) {
		terminal->utf8_codepoint = byte & 0x07;
		terminal->utf8_minimum = 0x10000;
		terminal->utf8_remaining = 3;
		return;
	}
	terminal_vt_put(terminal, TERMINAL_VT_REPLACEMENT);
}

int terminal_vt_init(terminal_vt *terminal, terminal_vt_cell *cells,
		     uint32_t rows, uint32_t columns)
{
	if (terminal == NULL || cells == NULL || rows == 0 || columns == 0 ||
	    rows > SIZE_MAX / columns / sizeof(*cells))
		return -1;
	*terminal = (terminal_vt) {
		.cells = cells,
		.rows = rows,
		.columns = columns,
	};
	memset(cells, 0, (size_t)rows * columns * sizeof(*cells));
	return 0;
}

void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count)
{
	for (size_t index = 0; index < byte_count; index++)
		terminal_vt_feed_byte(terminal, bytes[index]);
}
