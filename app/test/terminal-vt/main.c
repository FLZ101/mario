#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
		terminal_vt_cell alternate_cells[8];
		terminal_vt terminal;
		const terminal_vt_case *test_case = &cases[index];

		assert(test_case->rows * test_case->columns <=
		       sizeof(cells) / sizeof(cells[0]));
		assert(terminal_vt_init(&terminal, cells, alternate_cells,
					test_case->rows, test_case->columns) == 0);
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
		terminal_vt_cell alternate_cells[15];
		terminal_vt terminal;
		const terminal_vt_cursor_case *test_case = &cases[index];

		assert(terminal_vt_init(&terminal, cells, alternate_cells, 3, 5) == 0);
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
		terminal_vt_cell alternate_cells[12];
		terminal_vt terminal;
		const terminal_vt_erase_case *test_case = &cases[index];

		assert(terminal_vt_init(&terminal, cells, alternate_cells, 3, 4) == 0);
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
	terminal_vt_cell alternate_cells[16];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 4, 4) == 0);
	terminal_vt_feed(&terminal, input, sizeof(input) - 1);
	for (size_t cell = 0; cell < sizeof(cells) / sizeof(cells[0]); cell++)
		assert(cells[cell].codepoint == expected[cell]);
	assert(terminal.scroll_top == 1 && terminal.scroll_bottom == 2);
	assert(terminal.cursor_row == 2 && terminal.cursor_column == 3);
}

static void test_attributes_and_saved_cursor(void)
{
	static const uint8_t initial[] =
		"\x1b[31;44;1mA\x1b[4;7;92;105mB"
		"\x1b[22;24;27;39;49mC";
	static const uint8_t dec_save[] = "\x1b" "7\x1b[2;6H\x1b[0m\x1b" "8D";
	static const uint8_t csi_save[] =
		"\x1b[2;1H\x1b[32m\x1b[s\x1b[1;1H\x1b[0m\x1b[uE";
	terminal_vt_cell cells[12];
	terminal_vt_cell alternate_cells[12];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 2, 6) == 0);
	terminal_vt_feed(&terminal, initial, sizeof(initial) - 1);
	assert(cells[0].codepoint == 'A' && cells[0].foreground == 1 &&
	       cells[0].background == 4 &&
	       cells[0].attributes == TERMINAL_VT_ATTRIBUTE_BOLD);
	assert(cells[1].codepoint == 'B' && cells[1].foreground == 10 &&
	       cells[1].background == 13 &&
	       cells[1].attributes == (TERMINAL_VT_ATTRIBUTE_BOLD |
					TERMINAL_VT_ATTRIBUTE_UNDERLINE |
					TERMINAL_VT_ATTRIBUTE_INVERSE));
	assert(cells[2].codepoint == 'C' &&
	       cells[2].foreground == TERMINAL_VT_COLOR_DEFAULT &&
	       cells[2].background == TERMINAL_VT_COLOR_DEFAULT &&
	       cells[2].attributes == 0);

	terminal_vt_feed(&terminal, dec_save, sizeof(dec_save) - 1);
	assert(cells[3].codepoint == 'D' &&
	       cells[3].foreground == TERMINAL_VT_COLOR_DEFAULT &&
	       cells[3].background == TERMINAL_VT_COLOR_DEFAULT &&
	       cells[3].attributes == 0);
	terminal_vt_feed(&terminal, csi_save, sizeof(csi_save) - 1);
	assert(cells[6].codepoint == 'E' && cells[6].foreground == 2 &&
	       cells[6].background == TERMINAL_VT_COLOR_DEFAULT &&
	       cells[6].attributes == 0);
}

static void test_alternate_screen_and_reset(void)
{
	static const uint8_t primary[] = "pri";
	static const uint8_t enter[] = "\x1b[?1049halt";
	static const uint8_t leave[] = "\x1b[?1049lZ";
	static const uint8_t reset[] = "\x1b[?1049hQ\x1b" "c";
	terminal_vt_cell cells[8];
	terminal_vt_cell alternate_cells[8];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 2, 4) == 0);
	terminal_vt_feed(&terminal, primary, sizeof(primary) - 1);
	terminal_vt_feed(&terminal, enter, sizeof(enter) - 1);
	assert(terminal.using_alternate && terminal.cells == alternate_cells);
	assert(alternate_cells[0].codepoint == 'a' &&
	       alternate_cells[1].codepoint == 'l' &&
	       alternate_cells[2].codepoint == 't');
	assert(cells[0].codepoint == 'p' && cells[1].codepoint == 'r' &&
	       cells[2].codepoint == 'i');

	terminal_vt_feed(&terminal, leave, sizeof(leave) - 1);
	assert(!terminal.using_alternate && terminal.cells == cells);
	assert(cells[0].codepoint == 'p' && cells[1].codepoint == 'r' &&
	       cells[2].codepoint == 'i' && cells[3].codepoint == 'Z');

	terminal_vt_feed(&terminal, reset, sizeof(reset) - 1);
	for (size_t index = 0; index < sizeof(cells) / sizeof(cells[0]); index++) {
		assert(cells[index].codepoint == 0);
		assert(alternate_cells[index].codepoint == 0);
	}
	assert(!terminal.using_alternate && terminal.cells == cells);
	assert(terminal.cursor_row == 0 && terminal.cursor_column == 0);
	assert(terminal.scroll_top == 0 && terminal.scroll_bottom == 1);
	assert(terminal.foreground == TERMINAL_VT_COLOR_DEFAULT &&
	       terminal.background == TERMINAL_VT_COLOR_DEFAULT &&
	       terminal.attributes == 0);
}

static void test_resize(void)
{
	static const uint8_t primary[] = "ab\r\nde";
	static const uint8_t alternate[] = "\x1b[?1049hXY";
	terminal_vt_cell cells[6];
	terminal_vt_cell alternate_cells[6];
	terminal_vt_cell resized_cells[12];
	terminal_vt_cell resized_alternate_cells[12];
	terminal_vt_cell resized_scrollback_cells[24];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 2, 3) == 0);
	terminal_vt_feed(&terminal, primary, sizeof(primary) - 1);
	terminal_vt_feed(&terminal, alternate, sizeof(alternate) - 1);
	assert(terminal_vt_resize(&terminal, resized_cells, resized_alternate_cells,
		resized_scrollback_cells, 6, 3, 4) == 0);
	assert(terminal.rows == 3 && terminal.columns == 4);
	assert(terminal.using_alternate);
	assert(terminal.cells == resized_alternate_cells);
	assert(resized_alternate_cells[0].codepoint == 'X' &&
	       resized_alternate_cells[1].codepoint == 'Y');
	terminal_vt_feed(&terminal, (const uint8_t *)"\x1b[?1049l", 8);
	assert(terminal.cells == resized_cells);
	assert(resized_cells[0].codepoint == 'a' &&
	       resized_cells[1].codepoint == 'b' &&
	       resized_cells[4].codepoint == 'd' &&
	       resized_cells[5].codepoint == 'e');
}

static void test_scrollback_and_tabs(void)
{
	static const uint8_t lines[] = "one\r\ntwo\r\nthr\r\nfor\r\nfiv\r\nsix\r\nsev";
	static const uint8_t tabs[] = "\x1b[3g\x1b[5G\x1bH\rB\tC";
	terminal_vt_cell cells[8];
	terminal_vt_cell alternate_cells[8];
	terminal_vt_cell scrollback_cells[12];
	terminal_vt terminal;
	const terminal_vt_cell *line;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 2, 4) == 0);
	assert(terminal_vt_set_scrollback(&terminal, scrollback_cells, 3) == 0);
	terminal_vt_feed(&terminal, lines, sizeof(lines) - 1);
	assert(terminal.scrollback_count == 3);
	line = terminal_vt_scrollback_line(&terminal, 0);
	assert(line != NULL && line[0].codepoint == 't');
	line = terminal_vt_scrollback_line(&terminal, 1);
	assert(line != NULL && line[0].codepoint == 'f' && line[1].codepoint == 'o');
	line = terminal_vt_scrollback_line(&terminal, 2);
	assert(line != NULL && line[0].codepoint == 'f' && line[1].codepoint == 'i');
	assert(terminal_vt_scrollback_line(&terminal, 3) == NULL);

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 1, 8) == 0);
	terminal_vt_feed(&terminal, tabs, sizeof(tabs) - 1);
	assert(cells[0].codepoint == 'B' && cells[4].codepoint == 'C');
}

static void test_titles_and_direct_color(void)
{
	static const uint8_t semicolon_rgb[] =
		"\x1b[38;2;1;2;3;48;2;4;5;6mA";
	static const uint8_t colon_rgb[] =
		"\x1b[38:2::7:8:9;48:2::10:11:12mB";
	static const uint8_t bel_title[] = "\x1b]0;first title\x07";
	static const uint8_t st_title[] = "\x1b]2;second title\x1b\\";
	terminal_vt_cell cells[4];
	terminal_vt_cell alternate_cells[4];
	terminal_vt terminal;
	const uint8_t *title;
	uint16_t title_bytes;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 2, 2) == 0);
	terminal_vt_feed(&terminal, semicolon_rgb, sizeof(semicolon_rgb) - 1);
	terminal_vt_feed(&terminal, colon_rgb, sizeof(colon_rgb) - 1);
	assert(cells[0].foreground == (TERMINAL_VT_COLOR_RGB_TAG | 0x00010203U));
	assert(cells[0].background == (TERMINAL_VT_COLOR_RGB_TAG | 0x00040506U));
	assert(cells[1].foreground == (TERMINAL_VT_COLOR_RGB_TAG | 0x00070809U));
	assert(cells[1].background == (TERMINAL_VT_COLOR_RGB_TAG | 0x000a0b0cU));

	terminal_vt_feed(&terminal, bel_title, sizeof(bel_title) - 1);
	assert(terminal_vt_take_title(&terminal, &title, &title_bytes));
	assert(title_bytes == sizeof("first title") - 1);
	assert(memcmp(title, "first title", title_bytes) == 0);
	assert(!terminal_vt_take_title(&terminal, &title, &title_bytes));
	terminal_vt_feed(&terminal, st_title, sizeof(st_title) - 1);
	assert(terminal_vt_take_title(&terminal, &title, &title_bytes));
	assert(title_bytes == sizeof("second title") - 1);
	assert(memcmp(title, "second title", title_bytes) == 0);
}

static void test_dec_private_modes(void)
{
	static const uint8_t enable[] = "\x1b[?1h\x1b[?25l";
	static const uint8_t disable[] = "\x1b[?1l\x1b[?25h";
	terminal_vt_cell cells[2];
	terminal_vt_cell alternate_cells[2];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 1, 2) == 0);
	assert(terminal.cursor_visible && !terminal.application_cursor);
	terminal_vt_feed(&terminal, enable, sizeof(enable) - 1);
	assert(terminal.application_cursor && !terminal.cursor_visible);
	terminal_vt_feed(&terminal, disable, sizeof(disable) - 1);
	assert(!terminal.application_cursor && terminal.cursor_visible);
}

static void test_charset_designation(void)
{
	static const uint8_t text[] = "A\x1b(BZ";
	terminal_vt_cell cells[3];
	terminal_vt_cell alternate_cells[3];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 1, 3) == 0);
	terminal_vt_feed(&terminal, text, sizeof(text) - 1);
	assert(cells[0].codepoint == 'A' && cells[1].codepoint == 'Z' &&
	       cells[2].codepoint == 0);
}

int main(void)
{
	test_cases();
	test_csi_cursor();
	test_csi_erase();
	test_scroll_region();
	test_attributes_and_saved_cursor();
	test_alternate_screen_and_reset();
	test_resize();
	test_scrollback_and_tabs();
	test_titles_and_direct_color();
	test_dec_private_modes();
	test_charset_designation();
	puts("terminal-vt: basic controls ok");
	puts("terminal-vt: UTF-8 ok");
	puts("terminal-vt: CSI ok");
	puts("terminal-vt: attributes ok");
	puts("terminal-vt: resize ok");
	puts("terminal-vt: scrollback and tabs ok");
	puts("terminal-vt: titles and RGB ok");
	puts("terminal-vt: DEC modes ok");
	puts("terminal-vt: charset designation ok");
	return 0;
}
