#include <fs/tty/console.h>
#include <fs/tty/tty.h>
#include <fs/chrdev.h>

#include <lib/string.h>
#include <lib/stddef.h>
#include <lib/ctype.h>

#include <io.h>

static struct console console_table[NUM_CONSOLE];
static struct tty_struct console_tty_table[NUM_CONSOLE];

static volatile int fg_console = 0;

struct tty_struct *get_fg_tty()
{
	return &console_tty_table[fg_console];
}

struct console *get_fg_console()
{
	return &console_table[fg_console];
}

dev_t get_fg_console_dev()
{
	return MKDEV(TTY_MAJOR, TTY_MINOR_1 + fg_console);
}

static int is_fg(struct console *con)
{
	return get_fg_console() == con;
}

// /dev/console is /dev/tty1
int is_system_console(struct console *con)
{
	return con == console_table;
}

volatile int debug_console = 1;

#define DEBUG_PRINTK(...) do { \
	if (debug_console && !is_system_console(con)) \
		printk(__VA_ARGS__); \
} while (0)

static struct tty_struct *get_console_tty(struct console *con)
{
	for (int i = 0; i < NUM_CONSOLE; ++i)
		if (&console_table[i] == con)
			return &console_tty_table[i];
	unreachable();
}

enum Color {
	Black = 0,
	Blue,
	Green,
	Cyan,
	Red,
	Magenta,
	Brown,
	Light_Gray,
	Dark_Gray,
	Light_Blue,
	Light_Green,
	Light_Cyan,
	Light_Red,
	Light_Magenta,
	Yellow,
	White
};

/**
 * Translates a DEC Special Character (VT100) to its VGA CP437 equivalent.
 * These mappings apply when the terminal is in "Line Drawing Mode".
 * * @param c The incoming character (usually in the range 0x5F to 0x7E)
 * @return The CP437 byte to be written to VGA memory (0xB8000)
 */
static uint8_t translate_dec_to_cp437(char c) {
	switch (c) {
	// Line Drawing
	case 'q':
		return 0xC4; // ─ (Horizontal Bar)
	case 'x':
		return 0xB3; // │ (Vertical Bar)
	case 'l':
		return 0xDA; // ┌ (Top Left Corner)
	case 'k':
		return 0xBF; // ┐ (Top Right Corner)
	case 'm':
		return 0xC0; // └ (Bottom Left Corner)
	case 'j':
		return 0xD9; // ┘ (Bottom Right Corner)
	case 't':
		return 0xC3; // ├ (Left Tee)
	case 'u':
		return 0xB4; // ┤ (Right Tee)
	case 'v':
		return 0xC1; // ┴ (Bottom Tee)
	case 'w':
		return 0xC2; // ┬ (Top Tee)
	case 'n':
		return 0xC5; // ┼ (Cross/Plus)

	// Miscellaneous Graphics
	case 'a':
		return 0xB1; // ▒ (Checkerboard/Stipple)
	case '`':
		return 0x04; // ♦ (Diamond)
	case 'f':
		return 0xF8; // ° (Degree Symbol)
	case 'g':
		return 0xF1; // ± (Plus/Minus)
	case 'h':
		return 0x23; // # (Board - CP437 uses # or 0xDB)
	case 'i':
		return 0x07; // ∙ (Lantern/Bullet)
	case 'o':
		return 0x2D; // - (Scanline 1)
	case 'p':
		return 0x2D; // - (Scanline 3)
	case 'r':
		return 0x5F; // _ (Scanline 7)
	case 's':
		return 0x5F; // _ (Scanline 9)
	case 'y':
		return 0x3C; // < (Less than or equal - approx)
	case 'z':
		return 0x3E; // > (Greater than or equal - approx)
	case '{':
		return 0xE3; // π (Pi)
	case '|':
		return 0xF0; // ≡ (Not equal)
	case '}':
		return 0x9C; // £ (Pound sterling)
	case '~':
		return 0xFA; // · (Centered dot)

	// If no mapping exists, return the original character
	default:
		return c;
	}
}

static uint16_t makec(struct console *con, unsigned char c)
{
	uint8_t bg_color = con->color_inverted ? con->fg_color : con->bg_color;
	uint8_t fg_color = con->color_inverted ? con->bg_color : con->fg_color;
	if (con->bold && fg_color < 8)
		fg_color += 8;
	return (((bg_color << 4) | fg_color) << 8) | c;
}

#define SPACE makec(con, ' ')

static void hide_cursor(struct console *con) {
	con->cursor_hidden = 1;

	if (!is_fg(con))
		return;

	// Disable cursor by setting bit 5 (0x20) in Cursor Start register
	outb(0x3D4, 0x0A);          // Select Cursor Start register
	unsigned char start = inb(0x3D5);
	outb(0x3D5, start | 0x20);  // Set bit 5 to disable cursor
}

static void show_cursor(struct console *con) {
	con->cursor_hidden = 0;

	if (!is_fg(con))
		return;

	outb(0x3D4, 0x0A);
	unsigned char start = inb(0x3D5);
	outb(0x3D5, start & ~0x20); // Clear bit 5 to re-enable
}

static void move_cursor(struct console *con)
{
	if (!is_fg(con))
		return;

	unsigned int pos = N_COL * con->pos_y + con->pos_x;
	outb(0x3d4, 14);
	outb(0x3d5, pos >> 8);
	outb(0x3d4, 15);
	outb(0x3d5, pos);
}

static void get_cursor(unsigned int *pos_y, unsigned int *pos_x)
{
	uint16_t pos;

	outb(0x3D4, 0x0E);
	uint8_t high = inb(0x3D5);

	outb(0x3D4, 0x0F);
	uint8_t low = inb(0x3D5);

	pos = ((uint16_t)high << 8) | low;

	*pos_y = pos / N_COL;
	*pos_x = pos - *pos_y * N_COL;
}

static void set_pos(struct console *con, int y, int x)
{
	con->pos_y = MIN(MAX(y, 0), N_ROW - 1);
	con->pos_x = MIN(MAX(x, 0), N_COL - 1);
	move_cursor(con);
}

#define VIDEO_MEM ((uint16_t (*)[N_COL])(0xb8000 + KERNEL_BASE))

static void erase_screen(struct console *con, unsigned row_start, unsigned row_end)
{
	unsigned addr = SCREEN_ROW_BYTE_SIZE * row_start;
	unsigned n = N_COL * (row_end + 1 - row_start);

	memsetw((void *)con->mem + addr, SPACE, n);
	if (is_fg(con))
		memsetw((void *)VIDEO_MEM + addr, SPACE, n);
}

void clear_screen(struct console *con)
{
	erase_screen(con, 0, N_ROW - 1);
}

static void __erase_line(struct console *con, int start, int end)
{
	if (start > end || start > N_COL - 1 || end < 0)
		return;
	if (start < 0)
		start = 0;
	if (end > N_COL - 1)
		end = N_COL - 1;

	for (int i = start; i < end + 1; ++i) {
		con->mem[con->pos_y][i] = SPACE;
		if (is_fg(con))
			VIDEO_MEM[con->pos_y][i] = SPACE;
	}
}

static void erase_line(struct console *con)
{
	__erase_line(con, 0, N_COL - 1);
}

static void erase_line_left(struct console *con)
{
	__erase_line(con, 0, con->pos_x);
}

static void erase_line_right(struct console *con)
{
	__erase_line(con, con->pos_x, N_COL - 1);
}

static int in_scrolling_region(struct console *con, unsigned x)
{
	return con->scr_start <= x && x <= con->scr_end;
}

static void __scroll(struct console *con, unsigned start, unsigned end, int up)
{
	if (start > end || !in_scrolling_region(con, start) || !in_scrolling_region(con, end))
		return;

	unsigned dst, src, size;
	if (start < end) {
		size = end - start;
		if (up) {
			dst = start;
			src = start + 1;
		} else {
			src = start;
			dst = start + 1;
		}
		size *= SCREEN_ROW_BYTE_SIZE;
		src *= SCREEN_ROW_BYTE_SIZE;
		dst *= SCREEN_ROW_BYTE_SIZE;
		memmove((void *)con->mem + dst, (void *)con->mem + src, size);
		if (is_fg(con))
			memmove((void *)VIDEO_MEM + dst, (void *)VIDEO_MEM + src, size);
	}

	dst = up ? end : start;
	dst *= SCREEN_ROW_BYTE_SIZE;
	memsetw((void *)con->mem + dst, SPACE, N_COL);
	if (is_fg(con))
		memsetw((void *)VIDEO_MEM + dst, SPACE, N_COL);
}

static void delete_one_char(struct console *con, unsigned y, unsigned x)
{
	if (x >= N_COL)
		return;
	unsigned start = y * SCREEN_ROW_BYTE_SIZE + x * sizeof(uint16_t);
	unsigned end = y * SCREEN_ROW_BYTE_SIZE + (N_COL - 1) * sizeof(uint16_t);
	if (start < end) {
		unsigned sz = end - start;
		memmove((void *)con->mem + start, (void *)con->mem + start + sizeof(uint16_t), sz);
		if (is_fg(con))
			memmove((void *)VIDEO_MEM + start, (void *)VIDEO_MEM + start + sizeof(uint16_t), sz);
	}
	memsetw((void *)con->mem + end, SPACE, 1);
	if (is_fg(con))
		memsetw((void *)VIDEO_MEM + end, SPACE, 1);
}

static void insert_one_char(struct console *con, unsigned y, unsigned x)
{
	if (x >= N_COL)
		return;
	unsigned start = y * SCREEN_ROW_BYTE_SIZE + x * sizeof(uint16_t);
	unsigned end = y * SCREEN_ROW_BYTE_SIZE + (N_COL - 1) * sizeof(uint16_t);
	if (start < end) {
		unsigned sz = end - start;
		memmove((void *)con->mem + start + sizeof(uint16_t), (void *)con->mem + start, sz);
		if (is_fg(con))
			memmove((void *)VIDEO_MEM + start + sizeof(uint16_t), (void *)VIDEO_MEM + start, sz);
	}
	memsetw((void *)con->mem + start, SPACE, 1);
	if (is_fg(con))
		memsetw((void *)VIDEO_MEM + start, SPACE, 1);
}

// Return 1 if CSI b can be applied to c
int can_rep(unsigned char c) {
	// 1. Standard ASCII printables (Space through Tilde)
	if (c >= 0x20 && c <= 0x7E) {
		return 1;
	}

	// 2. CP437 Extended characters (Line drawing, math, etc.)
	// In many terminal emulators, these are valid targets for REP.
	if (c >= 0x80 && c <= 0xFE) {
		return 1;
	}

	// 3. Control characters (0-31, 127) are NEVER repeatable.
	// They perform actions, they don't leave glyphs.
	return 0;
}

void write_char(struct console *con, unsigned char c, int translate)
{
	if (con->pending_wrap) {
		if (c != '\n' && c != '\r') {
			con->pos_x = 0;
			con->pos_y++;
			if (con->pos_y == con->scr_end + 1) {
				con->pos_y = con->scr_end;
				__scroll(con, con->scr_start, con->scr_end, 1);
			}
		}
		con->pending_wrap = 0;
	}
	if (c == '\t') {
		con->pos_x += 8;
		con->pos_x &= ~7;
	} else if (c == '\r') {
		con->pos_x = 0;
	} else if (c == '\n') {
		con->pos_y++;
		if (con->pos_y == con->scr_end + 1) {
			con->pos_y = con->scr_end;
			__scroll(con, con->scr_start, con->scr_end, 1);
		} else if (con->pos_y > N_ROW - 1) {
			con->pos_y = N_ROW - 1;
		}
	} else if (c == '\b') {
		if (con->pos_x == 0)
			return;
		con->pos_x--;
	} else if (c >= ' ') {
		uint16_t _x = c;
		if (translate) {
			if (con->charset_G0 == '0') {
				_x = translate_dec_to_cp437(c);
			}
		}
		if (can_rep(_x))
			con->pgc = _x;

		_x = makec(con, _x);
		con->mem[con->pos_y][con->pos_x] = _x;
		if (is_fg(con))
			VIDEO_MEM[con->pos_y][con->pos_x] = _x;
		con->pos_x++;
	}

	if (con->pos_x >= N_COL) {
		con->pending_wrap = 1;
		con->pos_x = N_COL;
	}
	move_cursor(con);
}

/*
 * https://vt100.net/docs/vt102-ug/chapter5.html
 */

static unsigned int simple_atou(char *s)
{
	int i, n, res;

	n = strlen(s);
	if (n < 1 || n > 6)
		return (unsigned int)-1;

	res = 0;
	for (i = 0; n; n--, i++) {
		if (s[i] < '0' || s[i] > '9')
			return (unsigned int)-1;
		res = res*10 + s[i]-'0';
	}
	return res;
}

static int into_esc_buf(struct console *con, unsigned char c)
{
	if (con->esc_buf_p >= ESC_BUF_SIZE - 4)
		return 1;
	con->esc_buf[con->esc_buf_p++] = c;
	return 0;
}

static void reset_esc_buf(struct console *con)
{
	memset(con->esc_buf, 0, ESC_BUF_SIZE);
	con->esc_buf_p = 0;
}

// *s should be like "1", "1;2", "1;2;3"
static char *get_esc_arg(char **s)
{
	char *p, *q;

	for (p = *s; *p; ++p) {
		if (*p == ';') {
			*p = 0;
			break;
		}
		if ('0' <= *p && *p <= '9')
			continue;
		// other characters (esp. '\0') are invalid
		return NULL;
	}
	assert(!*p);

	if (*s == p)
		return NULL;
	q = *s;
	*s = p + 1;
	return q;
}

static int get_esc_uint_1(char **s, unsigned *a)
{
	char *arg = get_esc_arg(s);
	if (!arg)
		return 1;

	*a = simple_atou(arg);
	if (-1 == *a)
		return 1;
	return 0;
}

static int get_esc_uint_2(char **s, unsigned *a, unsigned *b)
{
	if (get_esc_uint_1(s, a))
		return 1;
	return get_esc_uint_1(s, b);
}

// set cursor position
static void csi_H(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned int pos_x, pos_y;

	if (get_esc_uint_2(&esc_buf, &pos_y, &pos_x))
		pos_x = pos_y = 1;

	--pos_y;
	--pos_x;
	set_pos(con, pos_y, pos_x);
}

// arrow keys
static void csi_ABCD(struct console *con, unsigned char c)
{
	unsigned int count;
	char *esc_buf = con->esc_buf;
	if (get_esc_uint_1(&esc_buf, &count))
		count = 1;

	switch (c) {
	case 'A':
		con->pos_y -= count;
		if (con->pos_y > N_ROW - 1)
			con->pos_y = 0;
		break;
	case 'B':
		con->pos_y += count;
		if (con->pos_y > N_ROW - 1)
			con->pos_y = N_ROW - 1;
		break;
	case 'C':
		con->pos_x += count;
		if (con->pos_x > N_COL - 1)
			con->pos_x = N_COL - 1;
		break;
	case 'D':
		con->pos_x -= count;
		if (con->pos_x > N_COL - 1)
			con->pos_x = 0;
		break;
	default:
		;
	}
	move_cursor(con);
}

static void csi_G(struct console *con)
{
	unsigned int count;
	char *esc_buf = con->esc_buf;
	if (get_esc_uint_1(&esc_buf, &count))
		count = 1;
	set_pos(con, con->pos_y, count - 1);
}

static void csi_d(struct console *con, unsigned char c)
{
	char *esc_buf = con->esc_buf;
	unsigned n;

	if (get_esc_uint_1(&esc_buf, &n))
		n = 1;

	if (c == 'e')
		n = con->pos_y + n;

	set_pos(con, n - 1, con->pos_x);
}

// save cursor position
static void csi_s(struct console *con)
{
	con->save_x = con->pos_x;
	con->save_y = con->pos_y;
}

// restore cursor position
static void csi_u(struct console *con)
{
	con->pos_x = con->save_x;
	con->pos_y = con->save_y;
	move_cursor(con);
}

static void do_csi_m(struct console *con, int action)
{
	switch (action) {
	case 0:
		con->fg_color = Light_Gray;
		con->bg_color = Black;
		con->bold = 0;
		con->color_inverted = 0;
		break;

	case 1:
		con->bold = 1;
		break;
	case 7:
		con->color_inverted = 1;
		break;

	case 27:
		con->color_inverted = 0;
		break;

	case 30:
		con->fg_color = Black;
		break;
	case 31:
		con->fg_color = Red;
		break;
	case 32:
		con->fg_color = Green;
		break;
	case 33:
		con->fg_color = Yellow;
		break;
	case 34:
		con->fg_color = Blue;
		break;
	case 35:
		con->fg_color = Magenta;
		break;
	case 36:
		con->fg_color = Cyan;
		break;
	case 37:
		con->fg_color = White;
		break;

	case 39:
		// Set foreground color to default
		con->fg_color = Light_Gray;
		break;

	case 40:
		con->bg_color = Black;
		break;
	case 41:
		con->bg_color = Red;
		break;
	case 42:
		con->bg_color = Green;
		break;
	case 43:
		con->bg_color = Yellow;
		break;
	case 44:
		con->bg_color = Blue;
		break;
	case 45:
		con->bg_color = Magenta;
		break;
	case 46:
		con->bg_color = Cyan;
		break;
	case 47:
		con->bg_color = White;
		break;

	case 49:
		// Set background color to default
		con->bg_color = Black;
		break;
	}
}

// SGR - SELECT GRAPHIC RENDITION
static void csi_m(struct console *con)
{
	char *arg = NULL;
	char *esc_buf = con->esc_buf;
	int action = 0;

	arg = get_esc_arg(&esc_buf);
	if (!arg) {
		do_csi_m(con, action);
		return;
	}

	do {
		action = simple_atou(arg);
		do_csi_m(con, action);
	} while ((arg = get_esc_arg(&esc_buf)));
}

static void csi_J(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 0;

	switch (m) {
	case 0:
		// Erase Below
		erase_screen(con, con->pos_y, N_ROW - 1);
		break;
	case 1:
		// Erase Above
		erase_screen(con, 0, con->pos_y);
		break;
	case 2:
		// Erase All
		clear_screen(con);
		con->pgc = '\0';
		break;
	}
}

static void csi_K(struct console *con)
{
	unsigned int m;
	char *esc_buf = con->esc_buf;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 0;

	switch (m) {
	case 0:
		erase_line_right(con);
		break;
	case 1:
		erase_line_left(con);
		break;
	case 2:
		erase_line(con);
		break;
	}
}

// Insert lines
static void csi_L(struct console *con)
{
	if (!in_scrolling_region(con, con->pos_y))
		return;

	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;
	for (unsigned i = 0; i < m; ++i)
		__scroll(con, con->pos_y, con->scr_end, 0);
	set_pos(con, con->pos_y, 0);
}

// Delete lines
static void csi_M(struct console *con)
{
	if (!in_scrolling_region(con, con->pos_y))
		return;

	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;
	for (unsigned i = 0; i < m; ++i)
		__scroll(con, con->pos_y, con->scr_end, 1);
	set_pos(con, con->pos_y, 0);
}

// Delete Ps Character(s) (default = 1) (DCH).
static void csi_P(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;
	for (unsigned i = 0; i < m; ++i)
		delete_one_char(con, con->pos_y, con->pos_x);
}

// Erase Ps Character(s) (default = 1) (ECH).
static void csi_X(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;
	__erase_line(con, con->pos_x, con->pos_x + m - 1);
}

// Insert Ps (Blank) Character(s) (default = 1) (ICH).
static void csi_at(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;
	for (unsigned i = 0; i < m; ++i)
		insert_one_char(con, con->pos_y, con->pos_x);
}

static void csi_n(struct console *con)
{
	unsigned int m;
	char *esc_buf = con->esc_buf;
	if (get_esc_uint_1(&esc_buf, &m))
		return;

	switch (m) {
	case 6: {
		struct tty_struct *tty = get_console_tty(con);

		char resp[32];
		sprintk(resp, "\033[%d;%dR", tty->winsize.ws_row, tty->winsize.ws_col);

		tty_receive_s(tty, resp);
		break;
	}
	}
}

void console_soft_reset(struct console *con);

static void use_alt_screen_buffer(struct console *con)
{
	// save
	memcpy(con->orig.mem, con->mem, SCREEN_BUF_BYTE_SIZE);

#define __do(NAME) con->orig.NAME = con->NAME
	__do(pos_x);
	__do(pos_y);
	__do(save_x);
	__do(save_y);
	__do(scr_start);
	__do(scr_end);
	__do(charset_G0);
	__do(charset_G1);
	__do(pgc);
	__do(fg_color);
	__do(bg_color);
	__do(cursor_hidden);
	__do(color_inverted);
	__do(bold);
#undef __do

	// clear the alternate screen buffer

	console_soft_reset(con);

	memsetw(con->mem, SPACE, SCREEN_BUF_SIZE);
	con->pos_x = 0;
	con->pos_y = 0;

	if (is_fg(con)) {
		memcpy(VIDEO_MEM, con->mem, SCREEN_BUF_BYTE_SIZE);
		move_cursor(con);
	}
}

static void use_normal_screen_buffer(struct console *con)
{
	// restore
	memcpy(con->mem, con->orig.mem, SCREEN_BUF_BYTE_SIZE);
#define __do(NAME) con->NAME = con->orig.NAME
	__do(pos_x);
	__do(pos_y);
	__do(save_x);
	__do(save_y);
	__do(scr_start);
	__do(scr_end);
	__do(charset_G0);
	__do(charset_G1);
	__do(pgc);
	__do(fg_color);
	__do(bg_color);
	__do(cursor_hidden);
	__do(color_inverted);
	__do(bold);
#undef __do

	if (is_fg(con)) {
		memcpy(VIDEO_MEM, con->mem, SCREEN_BUF_BYTE_SIZE);
		move_cursor(con);
	}
}

static void csi_q(struct console *con, unsigned char c)
{
	char *arg = NULL;
	char *esc_buf = con->esc_buf;
	switch (c) {
	case 'l': {
		while ((arg = get_esc_arg(&esc_buf))) {
			int action = simple_atou(arg);
			switch (action) {
			case 1:
				// Normal Cursor Keys (DECCKM), VT100.
				con->k.cursor_app = 0;
				break;
			case 7:
				// No Auto-Wrap Mode (DECAWM), VT100.
				break;
			case 25:
				hide_cursor(con);
				break;
			case 1049:
				con->pgc = '\0';
				use_normal_screen_buffer(con);
				break;
			}
		}
		break;
	}
	case 'h': {
		while ((arg = get_esc_arg(&esc_buf))) {
			int action = simple_atou(arg);
			switch (action) {
			case 1:
				// Application Cursor Keys (DECCKM), VT100.
				con->k.cursor_app = 1;
				break;
			case 7:
				// Auto-Wrap Mode (DECAWM), VT100.
				break;
			case 25:
				show_cursor(con);
				break;
			case 1049:
				use_alt_screen_buffer(con);
				break;
			}
		}
		break;
	}
	default:
		;
	}
}

// Set Scrolling Region [top;bottom] (default = full size of window) (DECSTBM), VT100.
static void csi_r(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned start, end;
	if (get_esc_uint_2(&esc_buf, &start, &end)) {
		start = 1;
		end = N_ROW;
	}

	if (1 <= start && start <= end && end <= N_ROW) {
		con->scr_start = start - 1;
		con->scr_end = end - 1;
	}
}

static void csi_l(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned n;
	if (get_esc_uint_1(&esc_buf, &n))
		return;

	switch (n) {
	case 4:
		// Replace mode
		break;
	}
}

static void csi_h(struct console *con)
{
	char *esc_buf = con->esc_buf;
	unsigned n;
	if (get_esc_uint_1(&esc_buf, &n))
		return;

	switch (n) {
	case 4:
		// Insert mode
		DEBUG_PRINTK("Not implemented: CSI %d h\n", n);
		break;
	}
}

// Repeat the preceding graphic character Ps times (REP).
static void csi_b(struct console *con)
{
	if (!con->pgc)
		return;

	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;
	for (unsigned i = 0; i < m; ++i)
		write_char(con, con->pgc, 0);
}

// Scroll up Ps lines (default = 1) (SU), VT420, ECMA-48.
static void csi_S(struct console *con)
{
	if (!in_scrolling_region(con, con->pos_y))
		return;

	char *esc_buf = con->esc_buf;
	unsigned m;
	if (get_esc_uint_1(&esc_buf, &m))
		m = 1;

	for (unsigned i = 0; i < m; ++i)
		__scroll(con, con->scr_start, con->scr_end, 1);
}

static void csi(struct console *con, unsigned char c)
{
	if (con->state == CSI_X)
		return;
	if (con->state == CSI_Q) {
		csi_q(con, c);
		return;
	}
	if (con->state == CSI_I) {
		if (c == 'p') {
			// Soft terminal reset (DECSTR), VT220 and up.
			console_soft_reset(con);
		}
		return;
	}

	switch (c) {
	case 'H': case 'f':
		csi_H(con);
		break;
	case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
		csi_ABCD(con, c);
		break;
	case 'G':
		csi_G(con);
		break;
	case 'd':
	case 'e':
		csi_d(con, c);
		break;
	case 's':
		csi_s(con);
		break;
	case 'u':
		csi_u(con);
		break;
	case 'm':
		csi_m(con);
		break;
	case 'J':
		csi_J(con);
		break;
	case 'K':
		csi_K(con);
		break;
	case 'L':
		csi_L(con);
		break;
	case 'M':
		csi_M(con);
		break;
	case 'P':
		csi_P(con);
		break;
	case 'X':
		csi_X(con);
		break;
	case '@':
		csi_at(con);
		break;
	case 'n':
		csi_n(con);
		break;
	case 'r':
		csi_r(con);
		break;
	case 'l':
		csi_l(con);
		break;
	case 'h':
		csi_h(con);
		break;
	case 'b':
		csi_b(con);
		break;
	case 'S':
		csi_S(con);
		break;
	case 't':
		break;
	default:
		DEBUG_PRINTK("Unknown csi: %s %c\n", con->esc_buf, c);
	}
}

// Index
static void esc_D(struct console *con)
{
	con->pos_y++;
	if (con->pos_y == con->scr_end + 1) {
		con->pos_y = con->scr_end;
		__scroll(con, con->scr_start, con->scr_end, 1);
	} else if (con->pos_y == N_ROW) {
		con->pos_y = N_ROW - 1;
	}
	move_cursor(con);
}

// Reverse Index
static void esc_M(struct console *con)
{
	con->pos_y--;
	if (con->pos_y == con->scr_start - 1) {
		con->pos_y = con->scr_start;
		__scroll(con, con->scr_start, con->scr_end, 0);
	} else if (con->pos_y == -1) {
		con->pos_y = 0;
	}
	move_cursor(con);
}

void console_reset(struct console *con);

void reset_screen(struct console *con)
{
	clear_screen(con);

	console_reset(con);

	move_cursor(con);
}

void console_write_char(struct console *con, unsigned char c)
{
	// '\0' is ignored
	if (!c)
		return;

#if 0
	if (c != ' ' && c != '\t') {
		if (isprint(c))
			DEBUG_PRINTK("%c", c);
		else
			DEBUG_PRINTK("\\d%d", c);
	}
#endif

	// Only accept 7-bit control characters
	switch (con->state) {
	case NORMAL:
		if (c == '\033') {
			con->state = ESC;
		} else {
			return write_char(con, c, 1);
		}
		break;

	case ESC:
		switch (c) {
		case '\033':
			break;
		case '[':
			reset_esc_buf(con);
			con->state = CSI;
			break;
		case ']':
			reset_esc_buf(con);
			con->state = OSC;
			break;
		case 'P':
			reset_esc_buf(con);
			con->state = DCS;
			break;
		case '(':
			con->state = CHARSET_G0;
			break;
		case ')':
			con->state = CHARSET_G1;
		case 'c':
			// Full Reset (RIS), VT100.
			reset_screen(con);
			con->state = NORMAL;
			break;
		case '=':
			// Application Keypad (DECKPAM)
			con->k.keypad_app = 1;
			con->state = NORMAL;
			break;
		case '>':
			// Normal Keypad (DECKPNM), VT100.
			con->k.keypad_app = 0;
			con->state = NORMAL;
			break;
		case '7':
			// Save Cursor (DECSC), VT100.
			csi_s(con);
			con->state = NORMAL;
			break;
		case '8':
			// Restore Cursor (DECRC), VT100.
			csi_u(con);
			con->state = NORMAL;
			break;
		case 'D':
			esc_D(con);
			con->state = NORMAL;
			break;
		case 'M':
			esc_M(con);
			con->state = NORMAL;
			break;
		default:
			DEBUG_PRINTK("Unknown escape sequence: %c(\\d%d)\n", c, c);
			con->state = NORMAL;
		}
		break;
	case CHARSET_G0:
		switch (c) {
		case '0':	// DEC Special Character and Line Drawing Set, VT100.
		case 'A':	// United Kingdom (UK), VT100.
		case 'B':	// United States (USASCII), VT100.
			con->charset_G0 = c;
			break;
		}
		con->state = NORMAL;
		break;
	case CHARSET_G1:
		switch (c) {
		case '0':	// DEC Special Character and Line Drawing Set, VT100.
		case 'A':	// United Kingdom (UK), VT100.
		case 'B':	// United States (USASCII), VT100.
			con->charset_G1 = c;
			break;
		}
		con->state = NORMAL;
		break;
	case CSI:
		if ('?' == c) {
			con->state = CSI_Q;
			break;
		}
		if ('>' == c) {
			con->state = CSI_X;
			break;
		}
		if ('!' == c) {
			con->state = CSI_I;
			break;
		}
	case CSI_I:
	case CSI_Q:
	case CSI_X:
		if (isalpha(c) || c == '@') {
			csi(con, c);
			con->state = NORMAL;
			break;
		}
		if (into_esc_buf(con, c))
			con->state = CSI_BAD;
		break;
	case CSI_BAD:
		if (isalpha(c)) {
			con->state = NORMAL;
		} else if (c == '\033') {
			con->state = ESC;
		}
		break;
	case OSC:
		if (c == 0x07) {
			// BEL
			con->state = NORMAL;
		} else if (c == '\033') {
			con->state = OSC_ST;
		}
		break;
	case OSC_ST:
		if (c != '\\') {
			DEBUG_PRINTK("'\\' is expected in OSC_ST state: \\d%d\n", c);
		}
		con->state = NORMAL;
		break;
	case DCS:
		if (c == '\033') {
			con->state = DCS_ST;
		}
		break;
	case DCS_ST:
		if (c != '\\') {
			DEBUG_PRINTK("'\\' is expected in DCS_ST state: \\d%d\n", c);
		}
		con->state = NORMAL;
		break;
	default:
		DEBUG_PRINTK("Unexpected char: \\d%d\n", c);
	}
	con->pending_wrap = 0;
}

void console_put_char(struct tty_struct *tty, unsigned char c)
{
	dev_t minor = MINOR(tty->dev);
	struct console *con = &console_table[minor - tty->driver->minor];

	console_write_char(con, c);
}

struct tty_driver console_driver = {
	.minor = TTY_MINOR_1,
	.n = NUM_CONSOLE,
	.tty_table = console_tty_table,
	.put_char = console_put_char,
};

void switch_fg_console(int i)
{
	irq_save();

	assert(0 <= i && i < NUM_CONSOLE);

	if (i == fg_console)
		return;

	struct console *con = &console_table[i];
	fg_console = i;

	memcpy(VIDEO_MEM, con->mem, SCREEN_BUF_BYTE_SIZE);
	move_cursor(con);

	if (con->cursor_hidden)
		hide_cursor(con);
	else
		show_cursor(con);

	// reset keyboard state
	con->k.v_flags = 0;
	con->k.v_key = 0;
	memset(con->esc_buf, 0, ESC_BUF_SIZE);
	con->esc_buf_p = 0;
	con->state = NORMAL;

	irq_restore();
}

void console_sync(struct console *con)
{
	memcpy(con->mem, VIDEO_MEM, SCREEN_BUF_BYTE_SIZE);
	get_cursor(&con->pos_y, &con->pos_x);
}

void console_soft_reset(struct console *con)
{
	con->bg_color = Black;
	con->fg_color = Light_Gray;
	con->save_x = 0;
	con->save_y = 0;
	con->scr_start = 0;
	con->scr_end = N_ROW - 1;
	con->cursor_hidden = 0;
	con->color_inverted = 0;
	con->bold = 0;
	con->pending_wrap = 0;
	con->pgc = '\0';
	con->charset_G0 = 'B';
	con->charset_G1 = 'B';
}

void console_reset(struct console *con)
{
	console_soft_reset(con);

	con->pos_x = 0;
	con->pos_y = 0;
	memsetw(con->mem, SPACE, SCREEN_BUF_SIZE);

	con->k = (struct kbd) { 0 };
	memset(con->esc_buf, 0, ESC_BUF_SIZE);
	con->esc_buf_p = 0;
	con->state = NORMAL;
}

void console_init()
{
	struct console *con;

	for (int i = 0; i < NUM_CONSOLE; ++i) {
		struct tty_struct *tty = &console_tty_table[i];
		con = &console_table[i];
		console_reset(con);

		tty->dev = MKDEV(TTY_MAJOR, TTY_MINOR_1 + i);
		tty->driver = &console_driver;
		tty->pgrp = 0;
		tty->session = 0;
		ring_buffer_init(&tty->read_buf);

		init_wait_queue(&tty->wait_read);

		tty->termios = default_termios;
		tty->winsize.ws_col = N_COL;
		tty->winsize.ws_row = N_ROW;

		tty->count = 0;
		tty->initialized = 1;
	}

	con = get_fg_console();
	console_sync(con);

	register_tty_driver(&console_driver);
}
