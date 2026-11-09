#include "vt.h"

#include <string.h>

#define TERMINAL_VT_REPLACEMENT 0xfffdu
#define TERMINAL_VT_STATE_TEXT 0U
#define TERMINAL_VT_STATE_ESCAPE 1U
#define TERMINAL_VT_STATE_CSI 2U

static void terminal_vt_set_cursor(terminal_vt *terminal, uint32_t row,
				   uint32_t column);

static void terminal_vt_scroll_region(terminal_vt *terminal)
{
	size_t row_bytes = (size_t)terminal->columns * sizeof(*terminal->cells);
	terminal_vt_cell *top = terminal->cells +
		(size_t)terminal->scroll_top * terminal->columns;
	uint32_t row_count = terminal->scroll_bottom - terminal->scroll_top;

	/* A one-row margin clears its only line instead of reading past it. */
	if (row_count != 0)
		memmove(top, top + terminal->columns, row_bytes * row_count);
	memset(terminal->cells + (size_t)terminal->scroll_bottom * terminal->columns,
		0, row_bytes);
}

static void terminal_vt_line_feed(terminal_vt *terminal)
{
	if (terminal->cursor_row == terminal->scroll_bottom) {
		terminal_vt_scroll_region(terminal);
		return;
	}
	if (terminal->cursor_row + 1 < terminal->rows) {
		terminal->cursor_row++;
		return;
	}
}

static void terminal_vt_put(terminal_vt *terminal, uint32_t codepoint)
{
	terminal->cells[(size_t)terminal->cursor_row * terminal->columns +
		terminal->cursor_column] = (terminal_vt_cell) {
			.codepoint = codepoint,
			.foreground = terminal->foreground,
			.background = terminal->background,
			.attributes = terminal->attributes,
		};
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

static void terminal_vt_reset_escape(terminal_vt *terminal)
{
	terminal->parser_state = TERMINAL_VT_STATE_TEXT;
	terminal->csi_parameter_count = 0;
	terminal->csi_ignored = 0;
	terminal->csi_private = 0;
}

static void terminal_vt_begin_csi(terminal_vt *terminal)
{
	terminal->parser_state = TERMINAL_VT_STATE_CSI;
	terminal->csi_parameter_count = 1;
	terminal->csi_ignored = 0;
	terminal->csi_private = 0;
	terminal->csi_parameters[0] = 0;
}

static void terminal_vt_save_cursor(terminal_vt *terminal)
{
	terminal->saved_cursor_row = terminal->cursor_row;
	terminal->saved_cursor_column = terminal->cursor_column;
	terminal->saved_foreground = terminal->foreground;
	terminal->saved_background = terminal->background;
	terminal->saved_attributes = terminal->attributes;
}

static void terminal_vt_restore_cursor(terminal_vt *terminal)
{
	terminal_vt_set_cursor(terminal, terminal->saved_cursor_row,
			       terminal->saved_cursor_column);
	terminal->foreground = terminal->saved_foreground;
	terminal->background = terminal->saved_background;
	terminal->attributes = terminal->saved_attributes;
}

static void terminal_vt_reset_rendition(terminal_vt *terminal)
{
	terminal->foreground = TERMINAL_VT_COLOR_DEFAULT;
	terminal->background = TERMINAL_VT_COLOR_DEFAULT;
	terminal->attributes = 0;
}

static uint32_t terminal_vt_csi_value(const terminal_vt *terminal,
				      uint8_t index)
{
	return index < terminal->csi_parameter_count ?
		terminal->csi_parameters[index] : 0;
}

static uint32_t terminal_vt_csi_count(const terminal_vt *terminal,
				      uint8_t index, uint32_t default_value)
{
	uint32_t value = terminal_vt_csi_value(terminal, index);

	return value == 0 ? default_value : value;
}

static void terminal_vt_clear_screen(terminal_vt *terminal, uint32_t mode)
{
	size_t cursor = (size_t)terminal->cursor_row * terminal->columns +
		terminal->cursor_column;
	size_t count = (size_t)terminal->rows * terminal->columns;

	if (mode == 0)
		memset(terminal->cells + cursor, 0,
			(count - cursor) * sizeof(*terminal->cells));
	else if (mode == 1)
		memset(terminal->cells, 0,
			(cursor + 1) * sizeof(*terminal->cells));
	else if (mode == 2)
		memset(terminal->cells, 0, count * sizeof(*terminal->cells));
}

static void terminal_vt_clear_line(terminal_vt *terminal, uint32_t mode)
{
	terminal_vt_cell *line = terminal->cells +
		(size_t)terminal->cursor_row * terminal->columns;

	if (mode == 0)
		memset(line + terminal->cursor_column, 0,
			(terminal->columns - terminal->cursor_column) * sizeof(*line));
	else if (mode == 1)
		memset(line, 0, (terminal->cursor_column + 1) * sizeof(*line));
	else if (mode == 2)
		memset(line, 0, terminal->columns * sizeof(*line));
}

static void terminal_vt_set_cursor(terminal_vt *terminal, uint32_t row,
				   uint32_t column)
{
	terminal->cursor_row = row >= terminal->rows ? terminal->rows - 1 : row;
	terminal->cursor_column = column >= terminal->columns ?
		terminal->columns - 1 : column;
}

static void terminal_vt_clear_buffer(const terminal_vt *terminal,
				     terminal_vt_cell *cells)
{
	memset(cells, 0, (size_t)terminal->rows * terminal->columns *
		sizeof(*cells));
}

static void terminal_vt_enter_alternate(terminal_vt *terminal)
{
	if (terminal->using_alternate)
		return;
	/* DEC 1049 preserves the visible primary state before clearing the alt grid. */
	terminal->primary_cursor_row = terminal->cursor_row;
	terminal->primary_cursor_column = terminal->cursor_column;
	terminal->primary_scroll_top = terminal->scroll_top;
	terminal->primary_scroll_bottom = terminal->scroll_bottom;
	terminal->primary_foreground = terminal->foreground;
	terminal->primary_background = terminal->background;
	terminal->primary_attributes = terminal->attributes;
	terminal->cells = terminal->alternate_cells;
	terminal->using_alternate = 1;
	terminal->cursor_row = 0;
	terminal->cursor_column = 0;
	terminal->scroll_top = 0;
	terminal->scroll_bottom = terminal->rows - 1;
	terminal_vt_reset_rendition(terminal);
	terminal_vt_clear_buffer(terminal, terminal->cells);
}

static void terminal_vt_leave_alternate(terminal_vt *terminal)
{
	if (!terminal->using_alternate)
		return;
	terminal->cells = terminal->primary_cells;
	terminal->using_alternate = 0;
	terminal->cursor_row = terminal->primary_cursor_row;
	terminal->cursor_column = terminal->primary_cursor_column;
	terminal->scroll_top = terminal->primary_scroll_top;
	terminal->scroll_bottom = terminal->primary_scroll_bottom;
	terminal->foreground = terminal->primary_foreground;
	terminal->background = terminal->primary_background;
	terminal->attributes = terminal->primary_attributes;
}

static void terminal_vt_reset(terminal_vt *terminal)
{
	/* RIS makes both screens blank, then resumes on the primary screen. */
	terminal_vt_clear_buffer(terminal, terminal->primary_cells);
	terminal_vt_clear_buffer(terminal, terminal->alternate_cells);
	terminal->cells = terminal->primary_cells;
	terminal->using_alternate = 0;
	terminal->cursor_row = 0;
	terminal->cursor_column = 0;
	terminal->scroll_top = 0;
	terminal->scroll_bottom = terminal->rows - 1;
	terminal_vt_reset_rendition(terminal);
	terminal_vt_save_cursor(terminal);
	terminal_vt_reset_utf8(terminal);
	terminal_vt_reset_escape(terminal);
}

static void terminal_vt_set_scroll_region(terminal_vt *terminal)
{
	uint32_t top = terminal_vt_csi_count(terminal, 0, 1);
	uint32_t bottom = terminal_vt_csi_count(terminal, 1, terminal->rows);

	if (top == 0 || bottom > terminal->rows || top >= bottom)
		return;
	terminal->scroll_top = top - 1;
	terminal->scroll_bottom = bottom - 1;
	terminal_vt_set_cursor(terminal, 0, 0);
}

static void terminal_vt_set_sgr(terminal_vt *terminal)
{
	for (uint8_t index = 0; index < terminal->csi_parameter_count; index++) {
		uint32_t parameter = terminal_vt_csi_value(terminal, index);

		if (parameter == 0)
			terminal_vt_reset_rendition(terminal);
		else if (parameter == 1)
			terminal->attributes |= TERMINAL_VT_ATTRIBUTE_BOLD;
		else if (parameter == 4)
			terminal->attributes |= TERMINAL_VT_ATTRIBUTE_UNDERLINE;
		else if (parameter == 7)
			terminal->attributes |= TERMINAL_VT_ATTRIBUTE_INVERSE;
		else if (parameter == 22)
			terminal->attributes &= ~TERMINAL_VT_ATTRIBUTE_BOLD;
		else if (parameter == 24)
			terminal->attributes &= ~TERMINAL_VT_ATTRIBUTE_UNDERLINE;
		else if (parameter == 27)
			terminal->attributes &= ~TERMINAL_VT_ATTRIBUTE_INVERSE;
		else if (parameter >= 30 && parameter <= 37)
			terminal->foreground = parameter - 30;
		else if (parameter == 39)
			terminal->foreground = TERMINAL_VT_COLOR_DEFAULT;
		else if (parameter >= 40 && parameter <= 47)
			terminal->background = parameter - 40;
		else if (parameter == 49)
			terminal->background = TERMINAL_VT_COLOR_DEFAULT;
		else if (parameter >= 90 && parameter <= 97)
			terminal->foreground = parameter - 90 + 8;
		else if (parameter >= 100 && parameter <= 107)
			terminal->background = parameter - 100 + 8;
	}
}

static void terminal_vt_execute_private_csi(terminal_vt *terminal,
					    uint8_t final)
{
	if (terminal_vt_csi_value(terminal, 0) != 1049)
		return;
	if (final == 'h')
		terminal_vt_enter_alternate(terminal);
	else if (final == 'l')
		terminal_vt_leave_alternate(terminal);
}

static void terminal_vt_execute_csi(terminal_vt *terminal, uint8_t final)
{
	uint32_t amount;

	if (terminal->csi_private) {
		terminal_vt_execute_private_csi(terminal, final);
		return;
	}
	switch (final) {
	case 'A':
		amount = terminal_vt_csi_count(terminal, 0, 1);
		terminal->cursor_row = amount > terminal->cursor_row ? 0 :
			terminal->cursor_row - amount;
		break;
	case 'B':
		amount = terminal_vt_csi_count(terminal, 0, 1);
		terminal->cursor_row = amount >= terminal->rows - terminal->cursor_row ?
			terminal->rows - 1 : terminal->cursor_row + amount;
		break;
	case 'C':
		amount = terminal_vt_csi_count(terminal, 0, 1);
		terminal->cursor_column = amount >=
			terminal->columns - terminal->cursor_column ?
			terminal->columns - 1 : terminal->cursor_column + amount;
		break;
	case 'D':
		amount = terminal_vt_csi_count(terminal, 0, 1);
		terminal->cursor_column = amount > terminal->cursor_column ? 0 :
			terminal->cursor_column - amount;
		break;
	case 'G':
		terminal_vt_set_cursor(terminal, terminal->cursor_row,
			terminal_vt_csi_count(terminal, 0, 1) - 1);
		break;
	case 'H':
	case 'f':
		terminal_vt_set_cursor(terminal,
			terminal_vt_csi_count(terminal, 0, 1) - 1,
			terminal_vt_csi_count(terminal, 1, 1) - 1);
		break;
	case 'J':
		terminal_vt_clear_screen(terminal, terminal_vt_csi_value(terminal, 0));
		break;
	case 'K':
		terminal_vt_clear_line(terminal, terminal_vt_csi_value(terminal, 0));
		break;
	case 'm':
		terminal_vt_set_sgr(terminal);
		break;
	case 'r':
		terminal_vt_set_scroll_region(terminal);
		break;
	case 's':
		terminal_vt_save_cursor(terminal);
		break;
	case 'u':
		terminal_vt_restore_cursor(terminal);
		break;
	}
}

static void terminal_vt_feed_csi(terminal_vt *terminal, uint8_t byte)
{
	if (byte == '?' && terminal->csi_parameter_count == 1 &&
	    terminal->csi_parameters[0] == 0 && !terminal->csi_private) {
		terminal->csi_private = 1;
		return;
	}
	if (byte >= '0' && byte <= '9') {
		uint32_t *value;

		if (terminal->csi_ignored)
			return;
		value = &terminal->csi_parameters[terminal->csi_parameter_count - 1];
		if (*value <= (UINT32_MAX - (byte - '0')) / 10)
			*value = *value * 10 + byte - '0';
		else
			terminal->csi_ignored = 1;
		return;
	}
	if (byte == ';') {
		if (terminal->csi_parameter_count == TERMINAL_VT_CSI_PARAMETERS)
			terminal->csi_ignored = 1;
		else if (!terminal->csi_ignored)
			terminal->csi_parameters[terminal->csi_parameter_count++] = 0;
		return;
	}
	if (byte >= 0x40 && byte <= 0x7e && !terminal->csi_ignored)
		terminal_vt_execute_csi(terminal, byte);
	terminal_vt_reset_escape(terminal);
}

static void terminal_vt_feed_escape(terminal_vt *terminal, uint8_t byte)
{
	if (byte == '[')
		terminal_vt_begin_csi(terminal);
	else if (byte == '7') {
		terminal_vt_reset_escape(terminal);
		terminal_vt_save_cursor(terminal);
	} else if (byte == '8') {
		terminal_vt_reset_escape(terminal);
		terminal_vt_restore_cursor(terminal);
	} else if (byte == 'c') {
		terminal_vt_reset(terminal);
	}
	else {
		terminal_vt_reset_escape(terminal);
		if (byte == 0x1b)
			terminal->parser_state = TERMINAL_VT_STATE_ESCAPE;
	}
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
	if (terminal->parser_state == TERMINAL_VT_STATE_ESCAPE) {
		terminal_vt_feed_escape(terminal, byte);
		return;
	}
	if (terminal->parser_state == TERMINAL_VT_STATE_CSI) {
		terminal_vt_feed_csi(terminal, byte);
		return;
	}
	if (byte == 0x1b) {
		terminal->parser_state = TERMINAL_VT_STATE_ESCAPE;
		return;
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

int terminal_vt_init(terminal_vt *terminal, terminal_vt_cell *primary_cells,
		     terminal_vt_cell *alternate_cells, uint32_t rows,
		     uint32_t columns)
{
	if (terminal == NULL || primary_cells == NULL || alternate_cells == NULL ||
	    primary_cells == alternate_cells || rows == 0 || columns == 0 ||
	    rows > SIZE_MAX / columns / sizeof(*primary_cells))
		return -1;
	*terminal = (terminal_vt) {
		.cells = primary_cells,
		.primary_cells = primary_cells,
		.alternate_cells = alternate_cells,
		.rows = rows,
		.columns = columns,
	};
	terminal_vt_reset(terminal);
	return 0;
}

void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count)
{
	for (size_t index = 0; index < byte_count; index++)
		terminal_vt_feed_byte(terminal, bytes[index]);
}
