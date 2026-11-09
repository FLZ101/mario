#ifndef TERMINAL_VT_H
#define TERMINAL_VT_H

#include <stddef.h>
#include <stdint.h>

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
	uint8_t utf8_remaining;
} terminal_vt;

/* Cells with codepoint zero are empty and render as the terminal background. */
int terminal_vt_init(terminal_vt *terminal, terminal_vt_cell *cells,
		     uint32_t rows, uint32_t columns);
void terminal_vt_feed(terminal_vt *terminal, const uint8_t *bytes,
		      size_t byte_count);

#endif
