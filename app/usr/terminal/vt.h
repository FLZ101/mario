#ifndef TERMINAL_VT_H
#define TERMINAL_VT_H

#include <stddef.h>
#include <stdint.h>

#define TERMINAL_VT_CSI_PARAMETERS 16U
#define TERMINAL_VT_TITLE_BYTES 256U
#define TERMINAL_VT_COLOR_DEFAULT UINT32_MAX
#define TERMINAL_VT_COLOR_RGB_TAG 0x01000000U
#define TERMINAL_VT_COLOR_RGB_MASK 0x00ffffffU
#define TERMINAL_VT_ATTRIBUTE_BOLD 0x01U
#define TERMINAL_VT_ATTRIBUTE_UNDERLINE 0x02U
#define TERMINAL_VT_ATTRIBUTE_INVERSE 0x04U

typedef struct {
	uint32_t codepoint;
	uint32_t foreground;
	uint32_t background;
	uint8_t attributes;
} terminal_vt_cell;

typedef struct {
	terminal_vt_cell *cells;
	terminal_vt_cell *primary_cells;
	terminal_vt_cell *alternate_cells;
	terminal_vt_cell *scrollback_cells;
	uint32_t rows;
	uint32_t columns;
	uint32_t scrollback_lines;
	uint32_t scrollback_head;
	uint32_t scrollback_count;
	uint32_t cursor_row;
	uint32_t cursor_column;
	uint32_t saved_cursor_row;
	uint32_t saved_cursor_column;
	uint32_t primary_cursor_row;
	uint32_t primary_cursor_column;
	uint32_t primary_scroll_top;
	uint32_t primary_scroll_bottom;
	uint32_t utf8_codepoint;
	uint32_t utf8_minimum;
	uint32_t scroll_top;
	uint32_t scroll_bottom;
	uint32_t csi_parameters[TERMINAL_VT_CSI_PARAMETERS];
	uint32_t foreground;
	uint32_t background;
	uint32_t saved_foreground;
	uint32_t saved_background;
	uint32_t primary_foreground;
	uint32_t primary_background;
	uint32_t osc_command;
	uint16_t title_bytes;
	uint16_t osc_title_bytes;
	uint16_t csi_colon_parameters;
	uint8_t utf8_remaining;
	uint8_t parser_state;
	uint8_t csi_parameter_count;
	uint8_t csi_ignored;
	uint8_t csi_private;
	uint8_t osc_separator_seen;
	uint8_t osc_ignored;
	uint8_t title_changed;
	uint8_t attributes;
	uint8_t saved_attributes;
	uint8_t primary_attributes;
	uint8_t using_alternate;
	uint8_t application_cursor;
	uint8_t cursor_visible;
	uint8_t tab_stops[256];
	uint8_t title[TERMINAL_VT_TITLE_BYTES];
	uint8_t osc_title[TERMINAL_VT_TITLE_BYTES];
} terminal_vt;

/* Cells with codepoint zero are empty and render as the terminal background. */
int terminal_vt_init(terminal_vt *terminal, terminal_vt_cell *primary_cells,
		     terminal_vt_cell *alternate_cells, uint32_t rows,
		     uint32_t columns);
int terminal_vt_set_scrollback(terminal_vt *terminal,
			       terminal_vt_cell *scrollback_cells,
			       uint32_t scrollback_lines);
int terminal_vt_resize(terminal_vt *terminal, terminal_vt_cell *primary_cells,
		       terminal_vt_cell *alternate_cells,
		       terminal_vt_cell *scrollback_cells,
		       uint32_t scrollback_lines, uint32_t rows, uint32_t columns);
const terminal_vt_cell *terminal_vt_scrollback_line(const terminal_vt *terminal,
					     uint32_t line);
int terminal_vt_take_title(terminal_vt *terminal, const uint8_t **title,
				   uint16_t *title_bytes);
void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count);

#endif
