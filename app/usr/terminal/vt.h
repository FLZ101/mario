#ifndef TERMINAL_VT_H
#define TERMINAL_VT_H

#include <stddef.h>
#include <stdint.h>

#define TERMINAL_VT_CSI_PARAMETERS 8U

typedef struct {
	uint32_t codepoint;
} terminal_vt_cell;

typedef struct {
	terminal_vt_cell *cells;
	uint32_t rows;
	uint32_t columns;
	uint32_t cursor_row;
	uint32_t cursor_column;
	uint32_t utf8_codepoint;
	uint32_t utf8_minimum;
	uint32_t scroll_top;
	uint32_t scroll_bottom;
	uint32_t csi_parameters[TERMINAL_VT_CSI_PARAMETERS];
	uint8_t utf8_remaining;
	uint8_t parser_state;
	uint8_t csi_parameter_count;
	uint8_t csi_ignored;
} terminal_vt;

/* Cells with codepoint zero are empty and render as the terminal background. */
int terminal_vt_init(terminal_vt *terminal, terminal_vt_cell *cells,
		     uint32_t rows, uint32_t columns);
void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count);

#endif
