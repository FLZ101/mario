#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <vt.h>

typedef struct {
	const uint8_t *bytes;
	size_t byte_count;
} terminal_vt_input;

typedef struct {
	const char *name;
	uint32_t rows;
	uint32_t columns;
	const terminal_vt_input *inputs;
	size_t input_count;
	const uint32_t *cells;
	uint32_t cursor_row;
	uint32_t cursor_column;
} terminal_vt_case;

static void test_cases(void)
{
	static const uint8_t ascii[] = "abc";
	static const terminal_vt_input ascii_inputs[] = {
		{ ascii, sizeof(ascii) - 1 },
	};
	static const uint32_t ascii_cells[] = { 'a', 'b', 'c', 0, 0, 0 };
	static const uint8_t controls[] = "ab\rc\n\bd";
	static const terminal_vt_input controls_inputs[] = {
		{ controls, sizeof(controls) - 1 },
	};
	static const uint32_t controls_cells[] = { 'c', 'b', 0, 'd', 0, 0 };
	static const uint8_t wrap[] = "abc";
	static const terminal_vt_input wrap_inputs[] = {
		{ wrap, sizeof(wrap) - 1 },
	};
	static const uint32_t wrap_cells[] = { 'a', 'b', 'c', 0, 0, 0 };
	static const uint8_t scroll[] = "one\r\nTWO\r\nxyz";
	static const terminal_vt_input scroll_inputs[] = {
		{ scroll, sizeof(scroll) - 1 },
	};
	static const uint32_t scroll_cells[] = { 'T', 'W', 'O', 0, 'x', 'y', 'z', 0 };
	static const uint8_t utf8_start[] = { 'A', 0xc3 };
	static const uint8_t utf8_end[] = { 0xa9, 0xf0, 0x9f, 0x98, 0x80 };
	static const terminal_vt_input utf8_inputs[] = {
		{ utf8_start, sizeof(utf8_start) },
		{ utf8_end, sizeof(utf8_end) },
	};
	static const uint32_t utf8_cells[] = { 'A', 0x00e9, 0x1f600, 0 };
	static const uint8_t malformed[] = { 0xc0, 'A', 0xe2, 'B' };
	static const terminal_vt_input malformed_inputs[] = {
		{ malformed, sizeof(malformed) },
	};
	static const uint32_t malformed_cells[] = { 0xfffd, 'A', 0xfffd, 'B', 0 };
	static const terminal_vt_case cases[] = {
		{ "ASCII", 2, 3, ascii_inputs, 1, ascii_cells, 1, 0 },
		{ "controls", 2, 3, controls_inputs, 1, controls_cells, 1, 1 },
		{ "wrap", 2, 3, wrap_inputs, 1, wrap_cells, 1, 0 },
		{ "scroll", 2, 4, scroll_inputs, 1, scroll_cells, 1, 3 },
		{ "UTF-8", 1, 4, utf8_inputs, 2, utf8_cells, 0, 3 },
		{ "malformed UTF-8", 1, 5, malformed_inputs, 1, malformed_cells, 0, 4 },
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		terminal_vt_cell cells[8];
		terminal_vt terminal;
		const terminal_vt_case *test_case = &cases[index];

		assert(test_case->rows * test_case->columns <=
		       sizeof(cells) / sizeof(cells[0]));
		assert(terminal_vt_init(&terminal, cells, test_case->rows,
					test_case->columns) == 0);
		for (size_t input = 0; input < test_case->input_count; input++)
			terminal_vt_feed(&terminal, test_case->inputs[input].bytes,
					 test_case->inputs[input].byte_count);
		for (uint32_t cell = 0; cell < test_case->rows * test_case->columns;
		     cell++)
			assert(cells[cell].codepoint == test_case->cells[cell]);
		assert(terminal.cursor_row == test_case->cursor_row);
		assert(terminal.cursor_column == test_case->cursor_column);
	}
}

int main(void)
{
	test_cases();
	puts("terminal-vt: basic controls ok");
	puts("terminal-vt: UTF-8 ok");
	return 0;
}
