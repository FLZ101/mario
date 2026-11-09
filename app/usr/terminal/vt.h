#ifndef TERMINAL_VT_H
#define TERMINAL_VT_H

#include <stddef.h>
#include <stdint.h>

#define TERMINAL_VT_CSI_PARAMETERS 8U
#define TERMINAL_VT_COLOR_DEFAULT 0xffU
#define TERMINAL_VT_ATTRIBUTE_BOLD 0x01U
#define TERMINAL_VT_ATTRIBUTE_UNDERLINE 0x02U
#define TERMINAL_VT_ATTRIBUTE_INVERSE 0x04U

typedef struct {
	uint32_t codepoint;
	uint8_t foreground;
	uint8_t background;
	uint8_t attributes;
} terminal_vt_cell;

typedef struct {
	terminal_vt_cell *cells;
	terminal_vt_cell *primary_cells;
	terminal_vt_cell *alternate_cells;
	uint32_t rows;
	uint32_t columns;
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
	uint8_t utf8_remaining;
	uint8_t parser_state;
	uint8_t csi_parameter_count;
	uint8_t csi_ignored;
	uint8_t csi_private;
	uint8_t foreground;
	uint8_t background;
	uint8_t attributes;
	uint8_t saved_foreground;
	uint8_t saved_background;
	uint8_t saved_attributes;
	uint8_t primary_foreground;
	uint8_t primary_background;
	uint8_t primary_attributes;
	uint8_t using_alternate;
} terminal_vt;

/* Cells with codepoint zero are empty and render as the terminal background. */
int terminal_vt_init(terminal_vt *terminal, terminal_vt_cell *primary_cells,
		     terminal_vt_cell *alternate_cells, uint32_t rows,
		     uint32_t columns);
int terminal_vt_resize(terminal_vt *terminal, terminal_vt_cell *primary_cells,
		       terminal_vt_cell *alternate_cells, uint32_t rows,
		       uint32_t columns);
void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count);

#endif
