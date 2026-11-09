#include "vt.h"

#include <string.h>

#define TERMINAL_VT_REPLACEMENT 0xfffdu
#define TERMINAL_VT_STATE_TEXT 0U
#define TERMINAL_VT_STATE_ESCAPE 1U
#define TERMINAL_VT_STATE_CSI 2U
#define TERMINAL_VT_STATE_OSC 3U
#define TERMINAL_VT_STATE_OSC_ESCAPE 4U

static void terminal_vt_set_cursor(terminal_vt *terminal, uint32_t row,
				   uint32_t column);

static void terminal_vt_scroll_region(terminal_vt *terminal)
{
	size_t row_bytes = (size_t)terminal->columns * sizeof(*terminal->cells);
	terminal_vt_cell *top = terminal->cells +
		(size_t)terminal->scroll_top * terminal->columns;
	uint32_t row_count = terminal->scroll_bottom - terminal->scroll_top;

	if (!terminal->using_alternate && terminal->scroll_top == 0 &&
	    terminal->scroll_bottom == terminal->rows - 1 &&
	    terminal->scrollback_lines != 0) {
		terminal_vt_cell *destination = terminal->scrollback_cells +
			(size_t)((terminal->scrollback_head + terminal->scrollback_count) %
			terminal->scrollback_lines) * terminal->columns;

		memcpy(destination, terminal->cells,
		       (size_t)terminal->columns * sizeof(*destination));
		if (terminal->scrollback_count < terminal->scrollback_lines)
			terminal->scrollback_count++;
		else
			terminal->scrollback_head = (terminal->scrollback_head + 1) %
				terminal->scrollback_lines;
	}

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
	if (byte == '\t') {
		for (uint32_t column = terminal->cursor_column + 1;
		     column < terminal->columns; column++) {
			if (column < sizeof(terminal->tab_stops) && terminal->tab_stops[column]) {
				terminal->cursor_column = column;
				return;
			}
		}
		terminal->cursor_column = terminal->columns - 1;
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
	terminal->csi_colon_parameters = 0;
	terminal->osc_command = 0;
	terminal->osc_title_bytes = 0;
	terminal->osc_separator_seen = 0;
	terminal->osc_ignored = 0;
}

static void terminal_vt_begin_csi(terminal_vt *terminal)
{
	terminal->parser_state = TERMINAL_VT_STATE_CSI;
	terminal->csi_parameter_count = 1;
	terminal->csi_ignored = 0;
	terminal->csi_private = 0;
	terminal->csi_colon_parameters = 0;
	terminal->csi_parameters[0] = 0;
}

static void terminal_vt_begin_osc(terminal_vt *terminal)
{
	terminal->parser_state = TERMINAL_VT_STATE_OSC;
	terminal->osc_command = 0;
	terminal->osc_title_bytes = 0;
	terminal->osc_separator_seen = 0;
	terminal->osc_ignored = 0;
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
	for (uint32_t column = 0; column < sizeof(terminal->tab_stops); column++)
		terminal->tab_stops[column] = column != 0 && column % 8 == 0;
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

static int terminal_vt_set_direct_color(const terminal_vt *terminal, uint8_t index,
					uint32_t *color, uint8_t *consumed)
{
	uint8_t rgb_index;

	if (index + 1 >= terminal->csi_parameter_count ||
	    terminal_vt_csi_value(terminal, index + 1) != 2)
		return 0;
	rgb_index = index + 2;
	/* Colon-form SGR permits an empty color-space field before RGB. */
	if (rgb_index < terminal->csi_parameter_count &&
	    (terminal->csi_colon_parameters & (1U << rgb_index)) != 0 &&
	    terminal_vt_csi_value(terminal, rgb_index) == 0)
		rgb_index++;
	if (rgb_index + 2 >= terminal->csi_parameter_count ||
	    terminal_vt_csi_value(terminal, rgb_index) > UINT8_MAX ||
	    terminal_vt_csi_value(terminal, rgb_index + 1) > UINT8_MAX ||
	    terminal_vt_csi_value(terminal, rgb_index + 2) > UINT8_MAX)
		return 0;
	*color = TERMINAL_VT_COLOR_RGB_TAG |
		(terminal_vt_csi_value(terminal, rgb_index) << 16) |
		(terminal_vt_csi_value(terminal, rgb_index + 1) << 8) |
		terminal_vt_csi_value(terminal, rgb_index + 2);
	*consumed = rgb_index + 3 - index;
	return 1;
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
		else if (parameter == 38 || parameter == 48) {
			uint32_t color;
			uint8_t consumed;

			if (terminal_vt_set_direct_color(terminal, index, &color, &consumed)) {
				if (parameter == 38)
					terminal->foreground = color;
				else
					terminal->background = color;
				index += consumed - 1;
			}
		}
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
	case 'g':
		if (terminal_vt_csi_value(terminal, 0) == 0 &&
		    terminal->cursor_column < sizeof(terminal->tab_stops))
			terminal->tab_stops[terminal->cursor_column] = 0;
		else if (terminal_vt_csi_value(terminal, 0) == 3)
			memset(terminal->tab_stops, 0, sizeof(terminal->tab_stops));
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
	if (byte == ';' || byte == ':') {
		if (terminal->csi_parameter_count == TERMINAL_VT_CSI_PARAMETERS)
			terminal->csi_ignored = 1;
		else if (!terminal->csi_ignored) {
			if (byte == ':')
				terminal->csi_colon_parameters |=
					1U << terminal->csi_parameter_count;
			terminal->csi_parameters[terminal->csi_parameter_count++] = 0;
		}
		return;
	}
	if (byte >= 0x40 && byte <= 0x7e && !terminal->csi_ignored)
		terminal_vt_execute_csi(terminal, byte);
	terminal_vt_reset_escape(terminal);
}

static void terminal_vt_finish_osc(terminal_vt *terminal)
{
	if (terminal->osc_separator_seen &&
	    (terminal->osc_command == 0 || terminal->osc_command == 2)) {
		memcpy(terminal->title, terminal->osc_title, terminal->osc_title_bytes);
		terminal->title_bytes = terminal->osc_title_bytes;
		terminal->title_changed = 1;
	}
	terminal_vt_reset_escape(terminal);
}

static void terminal_vt_feed_osc(terminal_vt *terminal, uint8_t byte)
{
	if (byte == 0x07) {
		terminal_vt_finish_osc(terminal);
		return;
	}
	if (byte == 0x1b) {
		terminal->parser_state = TERMINAL_VT_STATE_OSC_ESCAPE;
		return;
	}
	if (terminal->osc_ignored)
		return;
	if (!terminal->osc_separator_seen) {
		if (byte == ';') {
			terminal->osc_separator_seen = 1;
			return;
		}
		if (byte < '0' || byte > '9' ||
		    terminal->osc_command > (UINT32_MAX - (byte - '0')) / 10) {
			terminal->osc_ignored = 1;
			return;
		}
		terminal->osc_command = terminal->osc_command * 10 + byte - '0';
		return;
	}
	if (byte >= 0x20 && byte != 0x7f &&
	    terminal->osc_title_bytes < TERMINAL_VT_TITLE_BYTES)
		terminal->osc_title[terminal->osc_title_bytes++] = byte;
}

static void terminal_vt_feed_escape(terminal_vt *terminal, uint8_t byte)
{
	if (byte == '[')
		terminal_vt_begin_csi(terminal);
	else if (byte == ']')
		terminal_vt_begin_osc(terminal);
	else if (byte == 'H') {
		terminal_vt_reset_escape(terminal);
		if (terminal->cursor_column < sizeof(terminal->tab_stops))
			terminal->tab_stops[terminal->cursor_column] = 1;
	}
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
	if (terminal->parser_state == TERMINAL_VT_STATE_OSC) {
		terminal_vt_feed_osc(terminal, byte);
		return;
	}
	if (terminal->parser_state == TERMINAL_VT_STATE_OSC_ESCAPE) {
		if (byte == '\\')
			terminal_vt_finish_osc(terminal);
		else {
			terminal_vt_reset_escape(terminal);
			goto again;
		}
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

int terminal_vt_set_scrollback(terminal_vt *terminal,
			       terminal_vt_cell *scrollback_cells,
			       uint32_t scrollback_lines)
{
	if (terminal == NULL || (scrollback_lines != 0 && scrollback_cells == NULL) ||
	    scrollback_lines > SIZE_MAX / terminal->columns / sizeof(*scrollback_cells))
		return -1;
	terminal->scrollback_cells = scrollback_cells;
	terminal->scrollback_lines = scrollback_lines;
	terminal->scrollback_head = 0;
	terminal->scrollback_count = 0;
	if (scrollback_lines != 0)
		memset(scrollback_cells, 0, (size_t)scrollback_lines * terminal->columns *
			sizeof(*scrollback_cells));
	return 0;
}

static void terminal_vt_copy_resize_cells(terminal_vt_cell *destination,
					  uint32_t destination_columns,
					  const terminal_vt_cell *source,
					  uint32_t source_rows,
					  uint32_t source_columns,
					  uint32_t rows, uint32_t columns)
{
	uint32_t copied_rows = source_rows < rows ? source_rows : rows;
	uint32_t copied_columns = source_columns < columns ? source_columns : columns;

	for (uint32_t row = 0; row < copied_rows; row++)
		memcpy(destination + (size_t)row * destination_columns,
		       source + (size_t)row * source_columns,
		       (size_t)copied_columns * sizeof(*source));
}

int terminal_vt_resize(terminal_vt *terminal, terminal_vt_cell *primary_cells,
		       terminal_vt_cell *alternate_cells,
		       terminal_vt_cell *scrollback_cells,
		       uint32_t scrollback_lines, uint32_t rows, uint32_t columns)
{
	terminal_vt resized;
	size_t cell_count;
	uint32_t copied_scrollback;
	uint32_t first_scrollback;

	if (terminal == NULL || primary_cells == NULL || alternate_cells == NULL ||
	    primary_cells == alternate_cells || rows == 0 || columns == 0 ||
	    rows > SIZE_MAX / columns / sizeof(*primary_cells) ||
	    (scrollback_lines != 0 && scrollback_cells == NULL) ||
	    scrollback_lines > SIZE_MAX / columns / sizeof(*scrollback_cells))
		return -1;
	cell_count = (size_t)rows * columns;
	memset(primary_cells, 0, cell_count * sizeof(*primary_cells));
	memset(alternate_cells, 0, cell_count * sizeof(*alternate_cells));
	terminal_vt_copy_resize_cells(primary_cells, columns, terminal->primary_cells,
		terminal->rows, terminal->columns, rows, columns);
	terminal_vt_copy_resize_cells(alternate_cells, columns, terminal->alternate_cells,
		terminal->rows, terminal->columns, rows, columns);
	if (scrollback_lines != 0)
		memset(scrollback_cells, 0, (size_t)scrollback_lines * columns *
			sizeof(*scrollback_cells));
	copied_scrollback = terminal->scrollback_count < scrollback_lines ?
		terminal->scrollback_count : scrollback_lines;
	first_scrollback = terminal->scrollback_count - copied_scrollback;
	for (uint32_t line = 0; line < copied_scrollback; line++) {
		uint32_t source_line = (terminal->scrollback_head + first_scrollback + line) %
			terminal->scrollback_lines;

		terminal_vt_copy_resize_cells(scrollback_cells + (size_t)line * columns,
			columns, terminal->scrollback_cells +
			(size_t)source_line * terminal->columns, 1, terminal->columns, 1,
			columns);
	}
	resized = *terminal;
	resized.cells = terminal->using_alternate ? alternate_cells : primary_cells;
	resized.primary_cells = primary_cells;
	resized.alternate_cells = alternate_cells;
	resized.rows = rows;
	resized.columns = columns;
	resized.scrollback_cells = scrollback_cells;
	resized.scrollback_lines = scrollback_lines;
	resized.scrollback_head = 0;
	resized.scrollback_count = copied_scrollback;
	for (uint32_t column = terminal->columns;
	     column < columns && column < sizeof(resized.tab_stops); column++)
		resized.tab_stops[column] = column != 0 && column % 8 == 0;
	resized.cursor_row = resized.cursor_row < rows ? resized.cursor_row : rows - 1;
	resized.cursor_column = resized.cursor_column < columns ?
		resized.cursor_column : columns - 1;
	resized.saved_cursor_row = resized.saved_cursor_row < rows ?
		resized.saved_cursor_row : rows - 1;
	resized.saved_cursor_column = resized.saved_cursor_column < columns ?
		resized.saved_cursor_column : columns - 1;
	resized.primary_cursor_row = resized.primary_cursor_row < rows ?
		resized.primary_cursor_row : rows - 1;
	resized.primary_cursor_column = resized.primary_cursor_column < columns ?
		resized.primary_cursor_column : columns - 1;
	/* Old margins cannot describe the new grid, so reset them to full screen. */
	resized.scroll_top = 0;
	resized.scroll_bottom = rows - 1;
	resized.primary_scroll_top = 0;
	resized.primary_scroll_bottom = rows - 1;
	*terminal = resized;
	return 0;
}

const terminal_vt_cell *terminal_vt_scrollback_line(const terminal_vt *terminal,
					     uint32_t line)
{
	if (terminal == NULL || line >= terminal->scrollback_count)
		return NULL;
	return terminal->scrollback_cells +
		(size_t)((terminal->scrollback_head + line) % terminal->scrollback_lines) *
		terminal->columns;
}

int terminal_vt_take_title(terminal_vt *terminal, const uint8_t **title,
			   uint16_t *title_bytes)
{
	if (terminal == NULL || title == NULL || title_bytes == NULL || !terminal->title_changed)
		return 0;
	*title = terminal->title;
	*title_bytes = terminal->title_bytes;
	terminal->title_changed = 0;
	return 1;
}

void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count)
{
	for (size_t index = 0; index < byte_count; index++)
		terminal_vt_feed_byte(terminal, bytes[index]);
}
