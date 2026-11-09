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

static void test_csi_cursor(void)
{
	typedef struct {
		const terminal_vt_input *inputs;
		size_t input_count;
		uint32_t start_row;
		uint32_t start_column;
		uint32_t cursor_row;
		uint32_t cursor_column;
	} terminal_vt_cursor_case;
	static const uint8_t cup_start[] = "\x1b[2;";
	static const uint8_t cup_end[] = "4H";
	static const terminal_vt_input cup[] = {
		{ cup_start, sizeof(cup_start) - 1 },
		{ cup_end, sizeof(cup_end) - 1 },
	};
	static const uint8_t cha_bytes[] = "\x1b[3G";
	static const terminal_vt_input cha[] = {
		{ cha_bytes, sizeof(cha_bytes) - 1 },
	};
	static const uint8_t cuu_bytes[] = "\x1b[A";
	static const terminal_vt_input cuu[] = {
		{ cuu_bytes, sizeof(cuu_bytes) - 1 },
	};
	static const uint8_t cud_bytes[] = "\x1b[2B";
	static const terminal_vt_input cud[] = {
		{ cud_bytes, sizeof(cud_bytes) - 1 },
	};
	static const uint8_t cuf_bytes[] = "\x1b[2C";
	static const terminal_vt_input cuf[] = {
		{ cuf_bytes, sizeof(cuf_bytes) - 1 },
	};
	static const uint8_t cub_bytes[] = "\x1b[2D";
	static const terminal_vt_input cub[] = {
		{ cub_bytes, sizeof(cub_bytes) - 1 },
	};
	static const terminal_vt_cursor_case cases[] = {
		{ cup, 2, 2, 3, 1, 3 },
		{ cha, 1, 2, 4, 2, 2 },
		{ cuu, 1, 2, 3, 1, 3 },
		{ cud, 1, 0, 3, 2, 3 },
		{ cuf, 1, 1, 1, 1, 3 },
		{ cub, 1, 1, 3, 1, 1 },
	};

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		terminal_vt_cell cells[15];
		terminal_vt terminal;
		const terminal_vt_cursor_case *test_case = &cases[index];

		assert(terminal_vt_init(&terminal, cells, 3, 5) == 0);
		terminal.cursor_row = test_case->start_row;
		terminal.cursor_column = test_case->start_column;
		for (size_t input = 0; input < test_case->input_count; input++)
			terminal_vt_feed(&terminal, test_case->inputs[input].bytes,
					 test_case->inputs[input].byte_count);
		assert(terminal.cursor_row == test_case->cursor_row);
		assert(terminal.cursor_column == test_case->cursor_column);
	}
}

static void test_csi_erase(void)
{
	typedef struct {
		const uint8_t *sequence;
		size_t sequence_bytes;
		const uint32_t *cells;
	} terminal_vt_erase_case;
	static const uint8_t ed0[] = "\x1b[0J";
	static const uint8_t ed1[] = "\x1b[1J";
	static const uint8_t ed2[] = "\x1b[2J";
	static const uint8_t el0[] = "\x1b[0K";
	static const uint8_t el1[] = "\x1b[1K";
	static const uint8_t el2[] = "\x1b[2K";
	static const uint32_t ed0_cells[] = {
		'a', 'b', 'c', 0, 'd', 0, 0, 0, 0, 0, 0, 0,
	};
	static const uint32_t ed1_cells[] = {
		0, 0, 0, 0, 0, 0, 'f', 0, 'g', 'h', 'i', 0,
	};
	static const uint32_t empty_cells[12];
	static const uint32_t el0_cells[] = {
		'a', 'b', 'c', 0, 'd', 0, 0, 0, 'g', 'h', 'i', 0,
	};
	static const uint32_t el1_cells[] = {
		'a', 'b', 'c', 0, 0, 0, 'f', 0, 'g', 'h', 'i', 0,
	};
	static const uint32_t el2_cells[] = {
		'a', 'b', 'c', 0, 0, 0, 0, 0, 'g', 'h', 'i', 0,
	};
	static const terminal_vt_erase_case cases[] = {
		{ ed0, sizeof(ed0) - 1, ed0_cells },
		{ ed1, sizeof(ed1) - 1, ed1_cells },
		{ ed2, sizeof(ed2) - 1, empty_cells },
		{ el0, sizeof(el0) - 1, el0_cells },
		{ el1, sizeof(el1) - 1, el1_cells },
		{ el2, sizeof(el2) - 1, el2_cells },
	};
	static const uint8_t fill[] = "abc\r\ndef\r\nghi";
	static const uint8_t cursor[] = "\x1b[2;2H";

	for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
		terminal_vt_cell cells[12];
		terminal_vt terminal;
		const terminal_vt_erase_case *test_case = &cases[index];

		assert(terminal_vt_init(&terminal, cells, 3, 4) == 0);
		terminal_vt_feed(&terminal, fill, sizeof(fill) - 1);
		terminal_vt_feed(&terminal, cursor, sizeof(cursor) - 1);
		terminal_vt_feed(&terminal, test_case->sequence,
				 test_case->sequence_bytes);
		for (size_t cell = 0; cell < sizeof(cells) / sizeof(cells[0]); cell++)
			assert(cells[cell].codepoint == test_case->cells[cell]);
		assert(terminal.cursor_row == 1 && terminal.cursor_column == 1);
	}
}

static void test_scroll_region(void)
{
	static const uint8_t input[] =
		"top\r\nmid\r\nbot\r\nlow\x1b[2;3r\x1b[3;1Hone\r\ntwo";
	static const uint32_t expected[] = {
		't', 'o', 'p', 0, 'o', 'n', 'e', 0,
		't', 'w', 'o', 0, 'l', 'o', 'w', 0,
	};
	terminal_vt_cell cells[16];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, 4, 4) == 0);
	terminal_vt_feed(&terminal, input, sizeof(input) - 1);
	for (size_t cell = 0; cell < sizeof(cells) / sizeof(cells[0]); cell++)
		assert(cells[cell].codepoint == expected[cell]);
	assert(terminal.scroll_top == 1 && terminal.scroll_bottom == 2);
	assert(terminal.cursor_row == 2 && terminal.cursor_column == 3);
}

int main(void)
{
	test_cases();
	test_csi_cursor();
	test_csi_erase();
	test_scroll_region();
	puts("terminal-vt: basic controls ok");
	puts("terminal-vt: UTF-8 ok");
	puts("terminal-vt: CSI ok");
	return 0;
}
