#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <gui/font.h>

#include <vt.h>

static unsigned int failing_allocation;
static int measure_renderer;
static uint64_t renderer_pixels;
static unsigned int renderer_glyphs;

static void *renderer_calloc(size_t count, size_t bytes)
{
	if (failing_allocation && --failing_allocation == 0) {
		errno = ENOMEM;
		return NULL;
	}
	return calloc(count, bytes);
}

static void *renderer_malloc(size_t bytes)
{
	if (failing_allocation && --failing_allocation == 0) {
		errno = ENOMEM;
		return NULL;
	}
	return malloc(bytes);
}

static void renderer_fill(gui_surface *surface, gui_rect rectangle, gui_color color)
{
	int64_t left = rectangle.x < 0 ? 0 : rectangle.x;
	int64_t top = rectangle.y < 0 ? 0 : rectangle.y;
	int64_t right = (int64_t)rectangle.x + rectangle.w;
	int64_t bottom = (int64_t)rectangle.y + rectangle.h;

	if (right > surface->width)
		right = surface->width;
	if (bottom > surface->height)
		bottom = surface->height;
	if (measure_renderer && rectangle.w > 0 && rectangle.h > 0 && right > left && bottom > top)
		renderer_pixels += (right - left) * (bottom - top);
	gui_fill_rect(surface, rectangle, color);
}

static void renderer_text(gui_surface *surface, int position_x, int baseline,
			  gui_font *font, const char *text, gui_color color)
{
	if (measure_renderer)
		renderer_glyphs++;
	gui_font_draw_text(surface, position_x, baseline, font, text, color);
}

#define main terminal_program_main
#define calloc renderer_calloc
#define malloc renderer_malloc
#define gui_fill_rect renderer_fill
#define gui_font_draw_text renderer_text
#include "../../usr/terminal/main.c"
#undef gui_font_draw_text
#undef gui_fill_rect
#undef malloc
#undef calloc
#undef main

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
		{ "ASCII", 2, 3, ascii_inputs, 1, ascii_cells, 0, 2 },
		{ "controls", 2, 3, controls_inputs, 1, controls_cells, 1, 1 },
		{ "wrap", 2, 3, wrap_inputs, 1, wrap_cells, 0, 2 },
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

static void test_unsupported_csi(void)
{
	static const uint8_t text[] = "gui smoke base\r";
	static const uint8_t recovery[] = "\x1b[1;15H!";
	static const char *sequences[] = {
		"\x1b[?2026$p\x1b[?2048$p",
		"\x1b[0%m",
		"\x1b[31 m",
		"\x1b[?2026$ p",
		"\x1b[=1p",
		"\x1b[999999999999999999$p",
	};

	for (size_t index = 0; index < sizeof(sequences) / sizeof(sequences[0]); index++) {
		size_t length = strlen(sequences[index]);

		for (size_t split = 0; split <= length; split++) {
			terminal_vt_cell cells[20];
			terminal_vt_cell alternate_cells[20];
			terminal_vt_cell expected[20];
			terminal_vt terminal;
			const uint8_t *sequence = (const uint8_t *)sequences[index];

			assert(terminal_vt_init(&terminal, cells, alternate_cells, 1, 20) == 0);
			terminal_vt_feed(&terminal, text, sizeof(text) - 1);
			memcpy(expected, cells, sizeof(expected));
			terminal_vt_feed(&terminal, sequence, split);
			terminal_vt_feed(&terminal, sequence + split, length - split);
			assert(memcmp(cells, expected, sizeof(expected)) == 0);
			assert(terminal.cursor_row == 0 && terminal.cursor_column == 0);
			assert(terminal.foreground == TERMINAL_VT_COLOR_DEFAULT);
			terminal_vt_feed(&terminal, recovery, sizeof(recovery) - 1);
			assert(cells[14].codepoint == '!');
			assert(terminal.cursor_row == 0 && terminal.cursor_column == 15);
		}
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

static void test_delayed_wrap(void)
{
	static const uint8_t edge[] = "abc\x1b[31m";
	static const uint8_t next[] = "d";
	static const uint8_t repaint[] = "\x1b[1;2r\x1b[2;1HXYZ\x1b[1;1HQ";
	static const uint8_t newline[] = "\x1b[1;1H123\r\nR";
	static const uint8_t saved[] = "\x1b[1;1Habc\x1b" "7\rQ\x1b" "8Z";
	terminal_vt_cell cells[9];
	terminal_vt_cell alternate_cells[9];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 3, 3) == 0);
	terminal_vt_feed(&terminal, edge, sizeof(edge) - 1);
	assert(terminal.cursor_row == 0 && terminal.cursor_column == 2);
	assert(terminal.wrap_pending && cells[3].codepoint == 0);
	terminal_vt_feed(&terminal, next, sizeof(next) - 1);
	assert(!terminal.wrap_pending && terminal.cursor_row == 1 && terminal.cursor_column == 1);
	assert(cells[0].codepoint == 'a' && cells[3].codepoint == 'd');
	terminal_vt_feed(&terminal, repaint, sizeof(repaint) - 1);
	assert(cells[0].codepoint == 'Q' && cells[1].codepoint == 'b');
	assert(cells[3].codepoint == 'X' && cells[5].codepoint == 'Z');
	assert(cells[6].codepoint == 0);
	terminal_vt_feed(&terminal, newline, sizeof(newline) - 1);
	assert(cells[0].codepoint == '1' && cells[3].codepoint == 'R');
	assert(terminal.cursor_row == 1 && terminal.cursor_column == 1);
	terminal_vt_feed(&terminal, saved, sizeof(saved) - 1);
	assert(cells[0].codepoint == 'Q' && cells[3].codepoint == 'Z');
	assert(terminal.cursor_row == 1 && terminal.cursor_column == 1);
	puts("terminal-vt: delayed right-margin wrap preserves tmux peers ok");
}

static void test_tmux_resize_controls(void)
{
	static const uint8_t cursor_prefix[] = "\x1b[";
	static const uint8_t cursor_row[] = "1d";
	static const uint8_t cursor_clamp[] = "\x1b[0d\x1b[999999999d";
	static const uint8_t erase[] = "\x1b[2d\x1b[0X";
	static const uint8_t erase_clamp[] = "\x1b[999999999X";
	static const uint8_t margin[] = "\x1b[2;3r\x1b[4;3H\x1b[S";
	static const uint8_t scroll_clamp[] = "\x1b[0S\x1b[999999999S";
	terminal_vt_cell cells[20];
	terminal_vt_cell alternate_cells[20];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 4, 5) == 0);
	for (uint32_t row = 0; row < 4; row++)
		for (uint32_t column = 0; column < 5; column++)
			cells[row * 5 + column].codepoint = 'A' + row;
	terminal.cursor_row = 3;
	terminal.cursor_column = 3;
	terminal_vt_feed(&terminal, cursor_prefix, sizeof(cursor_prefix) - 1);
	terminal_vt_feed(&terminal, cursor_row, sizeof(cursor_row) - 1);
	assert(terminal.cursor_row == 0 && terminal.cursor_column == 3);
	terminal_vt_feed(&terminal, cursor_clamp, sizeof(cursor_clamp) - 1);
	assert(terminal.cursor_row == 3 && terminal.cursor_column == 3);
	terminal_vt_feed(&terminal, erase, sizeof(erase) - 1);
	assert(terminal.cursor_row == 1 && terminal.cursor_column == 3);
	assert(cells[7].codepoint == 'B' && cells[8].codepoint == 0 && cells[9].codepoint == 'B');
	terminal_vt_feed(&terminal, erase_clamp, sizeof(erase_clamp) - 1);
	assert(terminal.cursor_row == 1 && terminal.cursor_column == 3);
	assert(cells[7].codepoint == 'B' && cells[8].codepoint == 0 && cells[9].codepoint == 0);
	terminal_vt_feed(&terminal, margin, sizeof(margin) - 1);
	assert(terminal.cursor_row == 3 && terminal.cursor_column == 2);
	for (uint32_t column = 0; column < 5; column++) {
		assert(cells[column].codepoint == 'A');
		assert(cells[5 + column].codepoint == 'C');
		assert(cells[10 + column].codepoint == 0);
		assert(cells[15 + column].codepoint == 'D');
	}
	terminal_vt_feed(&terminal, scroll_clamp, sizeof(scroll_clamp) - 1);
	assert(terminal.cursor_row == 3 && terminal.cursor_column == 2);
	for (uint32_t column = 0; column < 5; column++) {
		assert(cells[column].codepoint == 'A' && cells[15 + column].codepoint == 'D');
		assert(cells[5 + column].codepoint == 0 && cells[10 + column].codepoint == 0);
	}
	puts("terminal-vt: tmux resize cursor, erase and scroll controls ok");
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

static void test_primary_reflow(void)
{
	terminal_vt_cell source_cells[6];
	terminal_vt_cell source_alternate_cells[6];
	terminal_vt_cell resized_cells[6];
	terminal_vt_cell resized_alternate_cells[6];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, source_cells,
		source_alternate_cells, 2, 3) == 0);
	terminal_vt_feed(&terminal, (const uint8_t *)"abc\r\nde",
		sizeof("abc\r\nde") - 1);
	assert(terminal_vt_resize(&terminal, resized_cells,
		resized_alternate_cells, NULL, 0, 3, 2) == 0);
	assert(terminal.rows == 3 && terminal.columns == 2);
	assert(terminal.cells == resized_cells);
	assert(resized_cells[0].codepoint == 'a' &&
	       resized_cells[1].codepoint == 'b');
	assert(resized_cells[2].codepoint == 'c' &&
	       resized_cells[3].codepoint == 0);
	assert(resized_cells[4].codepoint == 'd' &&
	       resized_cells[5].codepoint == 'e');

	assert(terminal_vt_init(&terminal, source_cells,
		source_alternate_cells, 2, 3) == 0);
	terminal_vt_feed(&terminal, (const uint8_t *)"ab\r\n ",
		sizeof("ab\r\n ") - 1);
	assert(terminal_vt_resize(&terminal, resized_cells,
		resized_alternate_cells, NULL, 0, 3, 2) == 0);
	assert(resized_cells[0].codepoint == 'a' &&
	       resized_cells[1].codepoint == 'b');
	assert(resized_cells[2].codepoint == ' ' &&
	       resized_cells[3].codepoint == 0);
	assert(resized_cells[4].codepoint == 0 &&
	       resized_cells[5].codepoint == 0);
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
	static const uint8_t text[] = "A\x1b(0qx\x1b(BZ";
	terminal_vt_cell cells[6];
	terminal_vt_cell alternate_cells[6];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate_cells, 1, 6) == 0);
	terminal_vt_feed(&terminal, text, sizeof(text) - 1);
	assert(cells[0].codepoint == 'A');
	assert(cells[1].codepoint == 0x2500);
	assert(cells[2].codepoint == 0x2502);
	assert(cells[3].codepoint == 'Z');
	assert(cells[4].codepoint == 0 && cells[5].codepoint == 0);
}

static void test_cursor_styles(void)
{
	static const char *invalid[] = {
		"\033[7 q", "\033[999999999999999999 q", "\033[1;2 q", "\033[1:2 q",
		"\033[5q", "\033[5  q", "\033[ 5q", "\033[?5 q", "\033[>5 q",
		"\033[5$q", "\033[5 m", "\033[5! q",
	};
	terminal_vt_cell cells[12];
	terminal_vt_cell alternate[12];
	terminal_vt terminal;

	assert(terminal_vt_init(&terminal, cells, alternate, 3, 4) == 0);
	assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BLOCK && terminal.cursor_blink);
	for (unsigned int style = 0; style <= 6; style++) {
		char sequence[16];
		size_t length = snprintf(sequence, sizeof(sequence), "\033[%u q", style);

		for (size_t split = 0; split <= length; split++) {
			assert(terminal_vt_init(&terminal, cells, alternate, 3, 4) == 0);
			terminal_vt_feed(&terminal, (const uint8_t *)"X\033[6 q", 6);
			terminal_vt_feed(&terminal, (const uint8_t *)sequence, split);
			terminal_vt_feed(&terminal, (const uint8_t *)sequence + split, length - split);
			assert(terminal.cursor_shape == (style <= 2 ? TERMINAL_VT_CURSOR_BLOCK :
				style <= 4 ? TERMINAL_VT_CURSOR_UNDERLINE : TERMINAL_VT_CURSOR_BAR));
			assert(terminal.cursor_blink == (style == 0 || (style & 1)));
			assert(cells[0].codepoint == 'X' && cells[1].codepoint == 0);
			assert(terminal.cursor_row == 0 && terminal.cursor_column == 1);
		}
	}
	for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); index++) {
		size_t length = strlen(invalid[index]);

		for (size_t split = 0; split <= length; split++) {
			assert(terminal_vt_init(&terminal, cells, alternate, 3, 4) == 0);
			terminal_vt_feed(&terminal, (const uint8_t *)"\033[6 q", 5);
			terminal_vt_feed(&terminal, (const uint8_t *)invalid[index], split);
			terminal_vt_feed(&terminal, (const uint8_t *)invalid[index] + split, length - split);
			assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BAR && !terminal.cursor_blink);
			for (size_t cell = 0; cell < 12; cell++)
				assert(cells[cell].codepoint == 0);
		}
	}
	terminal_vt_feed(&terminal, (const uint8_t *)"\033[ q", 4);
	assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BLOCK && terminal.cursor_blink);
	terminal_vt_feed(&terminal, (const uint8_t *)"\033[6 q\033[?12h", 11);
	assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BAR && terminal.cursor_blink);
	terminal_vt_feed(&terminal, (const uint8_t *)"\033[?12l\033[?25l", 12);
	assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BAR && !terminal.cursor_blink && !terminal.cursor_visible);
	terminal_vt_feed(&terminal, (const uint8_t *)"\033[?1049h\033[?1049l", 16);
	assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BAR && !terminal.cursor_blink);
	terminal_vt_feed(&terminal, (const uint8_t *)"\033c", 2);
	assert(terminal.cursor_shape == TERMINAL_VT_CURSOR_BLOCK && terminal.cursor_blink && terminal.cursor_visible);
	puts("terminal-vt: cursor styles and blink modes preserve cells across split reads");
}

static void assert_renderer_oracle(terminal_state *state)
{
	gui_surface *oracle = gui_surface_create(state->width, state->height);
	gui_surface content;
	const terminal_config *settings = terminal_settings(state);
	uint32_t cell_width = terminal_cell_width(state);
	uint32_t cell_height = terminal_cell_height(state);
	int baseline = terminal_baseline(state);

	assert(oracle != NULL);
	gui_fill_rect(oracle, (gui_rect) { 0, 0, state->width, state->height }, settings->background);
	content = (gui_surface) { .width = state->content.w, .height = state->content.h,
		.stride = oracle->stride, .pixels = oracle->pixels +
			(size_t)state->content.y * oracle->stride + state->content.x };
	for (uint32_t row = 0; row < state->vt.rows; row++)
		for (uint32_t column = 0; column < state->vt.columns; column++) {
			const terminal_vt_cell *cell = state->vt.cells + (size_t)row * state->vt.columns + column;
			gui_color foreground = terminal_color(state, cell->foreground, settings->foreground);
			gui_color background = terminal_color(state, cell->background, settings->background);
			int cell_x = column * cell_width;
			int cell_y = row * cell_height;

			if (cell->attributes & TERMINAL_VT_ATTRIBUTE_INVERSE) {
				gui_color color = foreground;

				foreground = background;
				background = color;
			}
			gui_fill_rect(&content, (gui_rect) { cell_x, cell_y, cell_width, cell_height }, background);
		}
	for (uint32_t row = 0; row < state->vt.rows; row++)
		for (uint32_t column = 0; column < state->vt.columns; column++) {
			const terminal_vt_cell *cell = state->vt.cells + (size_t)row * state->vt.columns + column;
			gui_color foreground = terminal_color(state, cell->foreground, settings->foreground);
			gui_color background = terminal_color(state, cell->background, settings->background);
			int cell_x = column * cell_width;
			int cell_y = row * cell_height;

			if (cell->attributes & TERMINAL_VT_ATTRIBUTE_INVERSE) {
				gui_color color = foreground;

				foreground = background;
				background = color;
			}
			(void)background;
			if (cell->codepoint != 0) {
				char bytes[5] = { 0 };

				terminal_encode_utf8(cell->codepoint, bytes);
				gui_font_draw_text(&content, cell_x, cell_y + baseline, state->font, bytes, foreground);
			}
			if (cell->attributes & TERMINAL_VT_ATTRIBUTE_UNDERLINE)
				gui_fill_rect(&content, (gui_rect) { cell_x, cell_y + cell_height - 2,
					cell_width, 1 }, foreground);
		}
	if (terminal_cursor_visible(state)) {
		gui_rect bounds = { state->vt.cursor_column * cell_width,
			state->vt.cursor_row * cell_height, cell_width, cell_height };

		if (state->vt.cursor_shape == TERMINAL_VT_CURSOR_UNDERLINE) {
			bounds.y += cell_height - 2;
			bounds.h = 2;
		} else if (state->vt.cursor_shape == TERMINAL_VT_CURSOR_BAR)
			bounds.w = 2;
		gui_fill_rect(&content, bounds, settings->cursor);
		if (state->vt.cursor_shape == TERMINAL_VT_CURSOR_BLOCK) {
			const terminal_vt_cell *cell = state->vt.cells +
				(size_t)state->vt.cursor_row * state->vt.columns + state->vt.cursor_column;
			gui_surface cursor = { .width = bounds.w, .height = bounds.h,
				.stride = content.stride, .pixels = content.pixels +
					(size_t)bounds.y * content.stride + bounds.x };

			if (cell->codepoint != 0) {
				char bytes[5] = { 0 };

				terminal_encode_utf8(cell->codepoint, bytes);
				gui_font_draw_text(&cursor, 0, baseline, state->font, bytes, settings->background);
			}
			if (cell->attributes & TERMINAL_VT_ATTRIBUTE_UNDERLINE)
				gui_fill_rect(&cursor, (gui_rect) { 0, cell_height - 2,
					cell_width, 1 }, settings->background);
		}
	}
	assert(!memcmp(state->pixels, oracle->pixels, (size_t)state->width * state->height * sizeof(gui_color)));
	gui_surface_destroy(oracle);
}

static void test_cursor_renderer(void)
{
	static terminal_state state = { .fd = -1, .master = -1, .window_id = 1, .configure_serial = 1 };
	uint64_t now = 1000;
	gui_protocol_header key = { .type = GUI_PROTOCOL_INPUT_EVENT };
	uint8_t payload[sizeof(gui_protocol_wire_input_event)] = { 0 };

	assert(terminal_resize_grid(&state, 320, 192) == 0);
	state.font = gui_font_load(TERMINAL_FONT_PATH, TERMINAL_FONT_HEIGHT);
	assert(state.font != NULL);
	terminal_vt_feed(&state.vt, (const uint8_t *)"W\033[H", 4);
	terminal_update_cursor_blink(&state, now);
	assert(terminal_render(&state) == 0);
	assert_renderer_oracle(&state);
	{
		unsigned int dark_pixels = 0;
		unsigned int light_pixels = 0;

		for (uint32_t row = 0; row < TERMINAL_CELL_HEIGHT; row++)
			for (uint32_t column = 0; column < TERMINAL_CELL_WIDTH; column++) {
				gui_color pixel = state.pixels[(size_t)row * state.width + column] & 0x00ffffffU;

				dark_pixels += pixel == TERMINAL_BACKGROUND;
				light_pixels += pixel == TERMINAL_CURSOR;
			}
		assert(dark_pixels > 0 && light_pixels > 0);
	}
	for (unsigned int style = 1; style <= 6; style++) {
		char sequence[16];
		size_t length = snprintf(sequence, sizeof(sequence), "\033[%u q", style);

		terminal_vt_feed(&state.vt, (const uint8_t *)sequence, length);
		terminal_reset_cursor_blink(&state);
		terminal_update_cursor_blink(&state, now);
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
		assert(state.render_damage.w <= 8 && state.render_damage.h <= 16);
		state.render_pending = 0;
		assert(terminal_cursor_timeout(&state, now, 0) == 0);
		assert(terminal_cursor_timeout(&state, now, 17) == 17);
		assert(terminal_cursor_timeout(&state, now, -1) == (style & 1 ? 500 : -1));
		terminal_update_cursor_blink(&state, now + 499);
		assert(!state.render_pending && terminal_cursor_visible(&state));
		assert(terminal_render(&state) == 0 && state.render_damage.w == 0);
		terminal_update_cursor_blink(&state, now + 500);
		assert(state.render_pending == (int)(style & 1));
		assert(terminal_cursor_visible(&state) == !(style & 1));
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
		state.render_pending = 0;
		terminal_update_cursor_blink(&state, now + 1000);
		assert(terminal_cursor_visible(&state));
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
		now += 2000;
	}
	terminal_vt_feed(&state.vt, (const uint8_t *)"\033[1 q", 5);
	terminal_reset_cursor_blink(&state);
	terminal_update_cursor_blink(&state, now);
	assert(terminal_render(&state) == 0);
	state.render_pending = 0;
	state.upload.active = 1;
	terminal_update_cursor_blink(&state, now + 500);
	assert(state.render_pending && !terminal_cursor_visible(&state));
	assert(terminal_cursor_timeout(&state, now + 1000, -1) == 0);
	assert(terminal_send_surface(&state) == 0 && state.render_pending && state.rendered_cursor_visible);
	state.upload.active = 0;
	assert(terminal_render(&state) == 0 && !state.rendered_cursor_visible);
	assert_renderer_oracle(&state);
	state.render_pending = 0;
	terminal_update_cursor_blink(&state, now + 1500);
	assert(!state.render_pending && !terminal_cursor_visible(&state));
	terminal_update_cursor_blink(&state, now + 2000);
	assert(state.render_pending && terminal_cursor_visible(&state));
	terminal_update_cursor_blink(&state, now + 2500);
	assert(!terminal_cursor_visible(&state));
	put32(payload, state.window_id);
	put32(payload + 4, GUI_INPUT_EVENT_KEY);
	put32(payload + 8, 'a');
	state.master = STDOUT_FILENO;
	assert(terminal_handle_message(&state, &key, payload, sizeof(payload)) == 0);
	state.master = -1;
	assert(terminal_cursor_visible(&state) && state.cursor_blink_deadline == 0);
	assert(state.render_pending && state.pty_input_bytes == 1 && state.pty_input[0] == 'a');
	terminal_update_cursor_blink(&state, now + 2500);
	assert(state.cursor_blink_deadline == now + 3000);
	terminal_vt_feed(&state.vt, (const uint8_t *)"\033[?25l", 6);
	terminal_update_cursor_blink(&state, now + 2600);
	assert(!terminal_cursor_visible(&state) && state.cursor_blink_deadline == 0);
	assert(terminal_cursor_timeout(&state, now + 10000, -1) == -1);
	assert(terminal_render(&state) == 0);
	assert_renderer_oracle(&state);
	gui_font_destroy(state.font);
	free(state.primary_cells);
	free(state.alternate_cells);
	free(state.scrollback_cells);
	free(state.rendered_cells);
	free(state.pixels);
	puts("terminal-vt: cursor shapes blink on bounded deadlines and preserve incremental pixels");
}

static void test_incremental_renderer(void)
{
	static terminal_state state = { .fd = -1, .master = -1, .window_id = 1, .configure_serial = 1 };
	static const char *sequences[] = {
		"\033[?25l", "\033[4;8H\033[4;7;38;2;239;68;68mUnicode: \xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80",
		"\033[0m\033[2J\033[H", "0123456789\r\nsecond\r\nthird\r\nfourth\r\nfifth\r\nsixth\r\nseventh\r\neighth\r\nninth\r\ntenth\r\n",
		"\033[2;5r\033[5;1Hscroll\nscroll\nscroll\033[r", "\033[?1049h\033[Halternate",
		"\033[?1049l\033[?25h\033[2;3H", "\033[2K\033[3;4H\033[48;2;19;83;137m\033[K",
		"\033[0m\033[1;1H\033[?25l", "\033c",
		"\033[4 q", "\033[6 q", "\033[2 q", "\033[?12h", "\033[?12l",
	};
	static const uint32_t glyphs[] = { 'j', 'W', 0x0301, 0x00c5, 0x4e2d, 0x1f600, 0x10ffff, 0 };
	gui_protocol_header configure = { .type = GUI_PROTOCOL_WINDOW_CONFIGURE };
	uint8_t payload[sizeof(gui_protocol_wire_window_configure)] = { 0 };
	static terminal_state previous;
	gui_color *saved_pixels;

	assert(terminal_resize_grid(&state, 320, 192) == 0);
	state.font = gui_font_load(TERMINAL_FONT_PATH, TERMINAL_FONT_HEIGHT);
	assert(state.font != NULL);
	state.vt.cursor_row = state.vt.cursor_column = 1;
	assert(terminal_render(&state) == 0);
	assert(state.rendered_cell_bytes == (size_t)state.vt.rows * state.vt.columns * sizeof(terminal_vt_cell));
	assert_renderer_oracle(&state);
	renderer_pixels = renderer_glyphs = 0;
	measure_renderer = 1;
	assert(terminal_render(&state) == 0);
	assert(renderer_pixels == 0 && renderer_glyphs == 0 && state.render_damage.w == 0);
	terminal_vt_feed(&state.vt, (const uint8_t *)"A", 1);
	assert(terminal_render(&state) == 0);
	assert(state.render_damage.w <= 24 && state.render_damage.h <= 32);
	assert(renderer_pixels <= 3 * 24 * 32 + 32 && renderer_glyphs <= 12);
	printf("terminal-drawing-work: single-cell pixels=%llu glyphs=%u rectangle=%dx%d snapshot=%u\n",
		(unsigned long long)renderer_pixels, renderer_glyphs,
		state.render_damage.w, state.render_damage.h, (unsigned int)state.rendered_cell_bytes);
	measure_renderer = 0;
	assert_renderer_oracle(&state);
	for (unsigned int index = 0; index < sizeof(sequences) / sizeof(sequences[0]); index++) {
		terminal_vt_feed(&state.vt, (const uint8_t *)sequences[index], strlen(sequences[index]));
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
	}
	for (unsigned int index = 0; index < 80; index++) {
		size_t cell_index = (index * 47) % ((size_t)state.vt.rows * state.vt.columns);

		state.vt.cells[cell_index].codepoint = glyphs[index % (sizeof(glyphs) / sizeof(glyphs[0]))];
		state.vt.cells[cell_index].background = TERMINAL_VT_COLOR_RGB_TAG | ((index * 89317) & 0xffffff);
		state.vt.cells[cell_index].attributes = index % 8;
		state.vt.cursor_row = (index * 3) % state.vt.rows;
		state.vt.cursor_column = (index * 7) % state.vt.columns;
		state.vt.cursor_visible = index % 2;
		state.vt.cursor_shape = index % 3;
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
	}
	previous = state;
	saved_pixels = malloc((size_t)state.width * state.height * sizeof(gui_color));
	assert(saved_pixels != NULL);
	memcpy(saved_pixels, state.pixels, (size_t)state.width * state.height * sizeof(gui_color));
	for (unsigned int allocation = 1; allocation <= 5; allocation++) {
		failing_allocation = allocation;
		assert(terminal_resize_grid(&state, 400, 256) < 0 && failing_allocation == 0);
		assert(!memcmp(&state, &previous, sizeof(state)));
		assert(!memcmp(state.pixels, saved_pixels, (size_t)state.width * state.height * sizeof(gui_color)));
		assert(terminal_render(&state) == 0);
		previous = state;
	}
	free(saved_pixels);
	put32(payload, 1);
	put64(payload + 4, 2);
	put32(payload + 12, 400);
	put32(payload + 16, 256);
	failing_allocation = 5;
	assert(terminal_handle_message(&state, &configure, payload, sizeof(payload)) == 1);
	assert(state.pending_configure_serial == 2 && state.configure_serial == 1 && state.width == 320);
	assert(state.surface_flow.retry_pending && state.pixels == previous.pixels);
	assert(terminal_apply_pending_configure(&state, -1) == 0 && state.width == 320);
	state.surface_flow.retry_pending = 0;
	assert(terminal_apply_pending_configure(&state, -1) == 0);
	assert(state.pending_configure_serial == 0 && state.configure_serial == 2 && state.width == 400);
	assert(state.vt.cursor_shape == previous.vt.cursor_shape && state.vt.cursor_blink == previous.vt.cursor_blink);
	assert(terminal_render(&state) == 0 && state.render_damage.w == 400 && state.render_damage.h == 256);
	assert_renderer_oracle(&state);
	gui_font_destroy(state.font);
	free(state.primary_cells);
	free(state.alternate_cells);
	free(state.scrollback_cells);
	free(state.rendered_cells);
	free(state.pixels);
	puts("terminal-vt: incremental pixels match full rendering and resize failures preserve state");
}

static void test_configured_renderer(void)
{
	static const unsigned int sizes[] = { 8, 20, 48 };
	static const char *sequences[] = {
		"\033[?25l\033[38;2;1;2;3;48;2;4;5;6mWj",
		"\033[4;7m\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80",
		"\033[0m\033[2J\033[H", "\033[44m\033[2K\033[1X",
		"\033[?1049hW\033[?1049l", "\033[?25h\033[H\033[2 q",
		"\033[4 q", "\033[6 q", "\033c",
	};

	for (size_t size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++) {
		terminal_state state = { .config = terminal_config_default };
		struct winsize size;

		state.config.font_size = sizes[size_index];
		state.config.padding = 8;
		state.config.foreground = 0x00123456;
		state.config.background = 0x00abcdef;
		state.config.cursor = 0x00010203;
		assert(terminal_setup_font(&state) == 0);
		assert(terminal_resize_grid(&state, 320, 192) == 0);
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
		for (size_t index = 0; index < sizeof(sequences) / sizeof(sequences[0]); index++) {
			terminal_vt_feed(&state.vt, (const uint8_t *)sequences[index], strlen(sequences[index]));
			assert(terminal_render(&state) == 0);
			assert_renderer_oracle(&state);
		}
		assert(terminal_resize_grid(&state, 640, 384) == 0);
		assert(state.vt.columns == 624 / terminal_cell_width(&state));
		assert(state.vt.rows == 368 / terminal_cell_height(&state));
		assert(terminal_winsize(&state, &size) == 0);
		assert(size.ws_xpixel == 624 && size.ws_ypixel == 368);
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
		assert(terminal_resize_grid(&state, terminal_cell_width(&state) + 2,
			terminal_cell_height(&state) + 4) == 0);
		assert(state.vt.columns == 1 && state.vt.rows == 1);
		assert(state.content.x == 1 && state.content.y == 2);
		assert(terminal_render(&state) == 0);
		assert_renderer_oracle(&state);
		gui_font_destroy(state.font);
		free(state.primary_cells);
		free(state.alternate_cells);
		free(state.scrollback_cells);
		free(state.rendered_cells);
		free(state.pixels);
	}
	puts("terminal-vt: configured fonts and colors preserve incremental rendering and cursor shapes");
}

static void test_blank_colors(void)
{
	static const char *sequences[] = { "\033[2J", "\033[2K", "\033[2X", "\033[2S" };
	terminal_vt_cell primary[4];
	terminal_vt_cell alternate[4];
	terminal_vt terminal;

	for (size_t index = 0; index < sizeof(sequences) / sizeof(sequences[0]); index++) {
		assert(terminal_vt_init(&terminal, primary, alternate, 1, 4) == 0);
		assert(primary[0].foreground == TERMINAL_VT_COLOR_DEFAULT &&
			primary[0].background == TERMINAL_VT_COLOR_DEFAULT);
		terminal_vt_feed(&terminal, (const uint8_t *)"\033[44m", 5);
		terminal_vt_feed(&terminal, (const uint8_t *)sequences[index], strlen(sequences[index]));
		assert(primary[0].codepoint == 0 && primary[0].background == 4);
		assert(primary[1].codepoint == 0 && primary[1].background == 4);
		terminal_vt_feed(&terminal, (const uint8_t *)"\033c", 2);
		assert(primary[0].background == TERMINAL_VT_COLOR_DEFAULT &&
			alternate[0].background == TERMINAL_VT_COLOR_DEFAULT);
	}
	puts("terminal-vt: blank cells use default colors and erases preserve the active background");
}

static void test_terminal_config(void)
{
	static const char settings[] =
		"# terminal settings\nforeground = #123456\ncolor1=#abcdef\n"
		"color_scheme=light\nfont_size=20\npadding=16\nopacity=50\ncursor=#010203\n";
	static const char invalid[] =
		"font_size=0\nfont_size=49\npadding=-1\npadding=3px\nopacity=-1\nopacity=101\n"
		"background=#bad\ncolor16=#ffffff\nunknown=1\ncolor_scheme=bad\nopaque=maybe\n";
	terminal_config config;
	terminal_state state = { 0 };
	gui_color foreground;
	gui_color background;
	char path[] = "/tmp/terminal-config.XXXXXX";
	int descriptor;

	assert(access("/root/.config/mario/terminal.conf", R_OK) == 0);
	assert(setenv("MARIO_TERMINAL_CONFIG", "/root/.config/mario/terminal.conf", 1) == 0);
	terminal_config_load(&config);
	assert(!memcmp(&config, &terminal_config_default, sizeof(config)));
	assert(unsetenv("MARIO_TERMINAL_CONFIG") == 0);
	assert(terminal_config_parse(&config, "", 0) == 0);
	assert(!memcmp(&config, &terminal_config_default, sizeof(config)));
	assert(config.padding == 0);
	assert(terminal_config_parse(&config, settings, sizeof(settings) - 1) == 0);
	assert(config.font_size == 20 && config.opacity == 50);
	assert(config.padding == 16);
	assert(config.foreground == 0x00123456 && config.background == 0x00f8fafc);
	assert(config.cursor == 0x00010203 && config.palette[1] == 0x00abcdef);
	assert(terminal_config_parse(&config, invalid, sizeof(invalid) - 1) == 0);
	assert(!memcmp(&config, &terminal_config_default, sizeof(config)));
	assert(terminal_config_parse(&config, "opaque=false", 12) == 0 && config.opacity == 80);
	assert(terminal_config_parse(&config, "opaque=true", 11) == 0 && config.opacity == 100);
	assert(terminal_config_parse(&config, "color_scheme=solarized-dark", sizeof("color_scheme=solarized-dark") - 1) == 0);
	assert(config.background == 0x00002b36 && config.palette[4] == 0x00268bd2);
	assert(terminal_config_parse(&config, settings, TERMINAL_CONFIG_MAX_BYTES + 1) < 0);
	descriptor = mkstemp(path);
	assert(descriptor >= 0 && write(descriptor, settings, sizeof(settings) - 1) == sizeof(settings) - 1);
	assert(close(descriptor) == 0 && setenv("MARIO_TERMINAL_CONFIG", path, 1) == 0);
	terminal_config_load(&state.config);
	assert(state.config.font_size == 20 && state.config.opacity == 50);
	assert(state.config.padding == 16);
	assert(terminal_setup_font(&state) == 0);
	assert(terminal_cell_width(&state) == 13 && terminal_cell_height(&state) == 24);
	assert(terminal_baseline(&state) == 19);
	assert(terminal_resize_grid(&state, 320, 192) == 0);
	assert(state.vt.columns == 22 && state.vt.rows == 6);
	terminal_cell_colors(&state, &state.vt.cells[0], &foreground, &background);
	assert(foreground == 0x00123456 && background == 0x00f8fafc);
	assert(terminal_color(&state, 1, 0) == 0x00abcdef);
	assert(terminal_color(&state, TERMINAL_VT_COLOR_RGB_TAG | 0x112233, 0) == 0x00112233);
	assert(terminal_render(&state) == 0);
	assert(state.pixels[0] == 0x00f8fafc);
	assert(state.pixels[16 * state.width + 16] == 0x00010203);
	assert_renderer_oracle(&state);
	gui_font_destroy(state.font);
	free(state.primary_cells);
	free(state.alternate_cells);
	free(state.scrollback_cells);
	free(state.rendered_cells);
	free(state.pixels);
	assert(unlink(path) == 0 && mkfifo(path, 0600) == 0);
	terminal_config_load(&config);
	assert(config.font_size == 14 && config.opacity == 100);
	assert(unlink(path) == 0);
	terminal_config_load(&config);
	assert(config.font_size == 14 && config.opacity == 100);
	assert(setenv("MARIO_TERMINAL_CONFIG", "", 1) == 0);
	terminal_config_load(&config);
	assert(config.font_size == 14);
	assert(unsetenv("MARIO_TERMINAL_CONFIG") == 0);
	puts("terminal-vt: config defaults, validation, themes and font geometry pass");
}

int main(void)
{
	test_terminal_config();
	test_configured_renderer();
	test_blank_colors();
	test_cases();
	test_csi_cursor();
	test_csi_erase();
	test_unsupported_csi();
	test_scroll_region();
	test_delayed_wrap();
	test_tmux_resize_controls();
	test_attributes_and_saved_cursor();
	test_alternate_screen_and_reset();
	test_resize();
	test_primary_reflow();
	test_scrollback_and_tabs();
	test_titles_and_direct_color();
	test_dec_private_modes();
	test_cursor_styles();
	test_charset_designation();
	puts("terminal-vt: basic controls ok");
	puts("terminal-vt: UTF-8 ok");
	puts("terminal-vt: CSI ok");
	puts("terminal-vt: unsupported CSI stays invisible across split reads");
	puts("terminal-vt: attributes ok");
	puts("terminal-vt: resize ok");
	puts("terminal-vt: scrollback and tabs ok");
	puts("terminal-vt: titles and RGB ok");
	puts("terminal-vt: DEC modes ok");
	puts("terminal-vt: charset designation ok");
	test_incremental_renderer();
	test_cursor_renderer();
	return 0;
}
