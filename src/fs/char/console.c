#include <fs/tty/console.h>
#include <fs/tty/tty.h>
#include <fs/chrdev.h>

#include <lib/string.h>
#include <lib/stddef.h>

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

static int is_fg(struct console *con)
{
	return get_fg_console() == con;
}

enum Color {
	Black,
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

#define MAKEC(con, c) ((((con->bg_color) << 4) | ((con->fg_color) & 0x0f)) << 8 | (c))
#define SPACE MAKEC(con, ' ')

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

static void get_cursor(unsigned int *pos_y, unsigned int *pos_x) {
	uint16_t pos;

	outb(0x3D4, 0x0E);
	uint8_t high = inb(0x3D5);

	outb(0x3D4, 0x0F);
	uint8_t low = inb(0x3D5);

	pos = ((uint16_t)high << 8) | low;

	*pos_y = pos / N_COL;
	*pos_x = pos - *pos_y * N_COL;
}

static void set_pos(struct console *con, int x, int y)
{
	if (0 <= x && x < N_COL)
		con->pos_x = x;
	if (0 <= y && y < N_ROW)
		con->pos_y = y;

	move_cursor(con);
}

static void reset_console(struct console *con)
{
	con->bg_color = Black;
	con->fg_color = Light_Gray;
	memsetw(con->mem, SPACE, N_ROW * N_COL);
	set_pos(con, 0, 0);

	if (is_fg(con))
		memsetw(VIDEO_MEM, SPACE, N_ROW * N_COL);
}

static void scroll_one_line(struct console *con)
{
	memmove((void *)con->mem, (void *)(con->mem + N_COL), N_COL * (N_ROW - 1) * sizeof (uint16_t));
	memsetw((void *)(con->mem + N_COL * (N_ROW - 1) * sizeof (uint16_t)), SPACE, N_COL);

	if (is_fg(con)) {
		memmove((void *)VIDEO_MEM, (void *)(VIDEO_MEM + N_COL), N_COL * (N_ROW - 1) * sizeof (uint16_t));
		memsetw((void *)(VIDEO_MEM + N_COL * (N_ROW - 1) * sizeof (uint16_t)), SPACE, N_COL);
	}
}

void write_char(struct console *con, unsigned char c)
{
	if (c == '\t') {
		con->pos_x += 8;
		con->pos_x &= ~7;
	} else if (c == '\n') {
		con->pos_x = 0;
		con->pos_y++;
	} else if (c == '\b') {
		if (con->pos_x == 0)
			return;
		con->pos_x--;
		con->mem[con->pos_y][con->pos_x] = SPACE;
		if (is_fg(con))
			VIDEO_MEM[con->pos_y][con->pos_x] = SPACE;
	} else if (c >= ' ') {
		con->mem[con->pos_y][con->pos_x] = MAKEC(con, c);
		if (is_fg(con))
			VIDEO_MEM[con->pos_y][con->pos_x] = MAKEC(con, c);
		con->pos_x++;
	}

	if (con->pos_x >= N_COL) {
		con->pos_x = 0;
		con->pos_y++;
	}
	if (con->pos_y >= N_ROW) {
		con->pos_y = N_ROW - 1;
		scroll_one_line(con);
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

static int isalpha(unsigned char c)
{
	if ('a' <= c && c <= 'z')
		return 1;
	if ('A' <= c && c <= 'Z')
		return 1;
	return 0;
}

static int into_esc_buf(struct console *con, unsigned char c)
{
	con->esc_buf[con->esc_buf_p++] = c;

	if (con->esc_buf_p == ESC_BUF_SIZE - 1)
		return 1;
	return 0;
}

static void reset_esc_buf(struct console *con)
{
	memset(con->esc_buf, 0, ESC_BUF_SIZE);
	con->esc_buf_p = 0;
}

static char *get_esc_arg(char *str)
{
	int i;
	char *p, *tmp;
	static char *s;

	if (str)
		s = str;

	for (p = s, i = 0; *s; i++) {
		tmp = s++;
		if (*tmp == ';' || isalpha(*tmp)) {
			*tmp = 0;
			break;
		}
	}
	if (!i)
		return NULL;
	return p;
}

// set cursor position
static void csi_H(struct console *con)
{
	char *tmp;
	unsigned int pos_x, pos_y;

	pos_x = pos_y = 0;

	tmp = get_esc_arg(con->esc_buf);
	if (!tmp)
		goto __set_pos;
	pos_x = simple_atou(tmp);

	tmp = get_esc_arg(NULL);
	if (!tmp)
		goto __set_pos;
	pos_y = simple_atou(tmp);

__set_pos:
	if (pos_x == 0)
		pos_x = 1;
	if (pos_y == 0)
		pos_y = 1;
	--pos_x;
	--pos_y;
	set_pos(con, pos_x, pos_y);
}

// arrow keys
static void csi_ABCD(struct console *con, unsigned char c)
{
	char *tmp;
	unsigned int count;

	count = 1;

	tmp = get_esc_arg(con->esc_buf);
	if (!tmp)
		goto __move;
	count = simple_atou(tmp);

__move:
	switch (c) {
	case 'A':
	case 'F':
		con->pos_y -= count;
		if (con->pos_y > N_ROW - 1)
			con->pos_y = 0;
		if (c == 'F')
			con->pos_x = 0;
		break;
	case 'B':
	case 'E':
		con->pos_y += count;
		if (con->pos_y > N_ROW - 1)
			con->pos_y = N_ROW - 1;
		if (c == 'E')
			con->pos_x = 0;
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

// save cursor position
static void csi_s(struct console *con)
{
	if (get_esc_arg(con->esc_buf))
		return;
	con->save_x = con->pos_x;
	con->save_y = con->pos_y;
}

// restore cursor position
static void csi_u(struct console *con)
{
	if (get_esc_arg(con->esc_buf))
		return;
	con->pos_x = con->save_x;
	con->pos_y = con->save_y;
	move_cursor(con);
}

// SGR - SELECT GRAPHIC RENDITION
static void csi_m(struct console *con)
{
	char *arg = NULL;
	while ((arg = get_esc_arg(con->esc_buf))) {
		int action = simple_atou(arg);
		switch (action) {
		case 0:
			con->fg_color = Light_Gray;
			con->bg_color = Black;
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
		}
	}
}

static void csi_J(struct console *con)
{

}

static void csi_K(struct console *con)
{

}

static void csi_L(struct console *con)
{
}

static void csi(struct console *con, unsigned char c)
{
	switch (c) {
	case 'H': case 'f':
		csi_H(con);
		break;
	case 'A': case 'B': case 'C': case 'D': case 'E': case 'F':
		csi_ABCD(con, c);
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
	}
}

void console_write_char(struct console *con, unsigned char c)
{
	// '\0' is ignored
	if (!c)
		return;

	enum _state {
		NORMAL,
		ESC,
		CSI,
		BAD
	};

	static enum _state state;

	switch (state) {
	case NORMAL:
		if (c == '\033')
			state = ESC;
		else
			write_char(con, c);
		break;
	case ESC:
		if (c == '[') {
			reset_esc_buf(con);
			state = CSI;
		} else if (c == 'c') {
			state = NORMAL;
			reset_console(con);
		} else {
			state = NORMAL;
			write_char(con, c);
		}
		break;
	case CSI:
		if (isalpha(c)) {
			csi(con, c);
			state = NORMAL;
		}
		if (into_esc_buf(con, c))
			state = BAD;
		break;
	case BAD:
		if (isalpha(c))
			state = NORMAL;
		break;
	default:
		;
	}
}

void console_put_char(struct tty_struct *tty, unsigned char c)
{
	dev_t minor = MINOR(tty->dev);
	struct console *con = &console_table[minor - 1];

	console_write_char(con, c);
}

static int console_write(struct tty_struct *tty, unsigned char *buf, int count)
{
	for (int i = 0; i < count; ++i) {
		unsigned char c = get_fs_byte(buf + i);
		console_put_char(tty, c);
	}
	return count;
}

struct tty_driver console_driver = {
	.minor = TTY_MINOR_1,
	.n = NUM_CONSOLE,
	.tty_table = console_tty_table,
	.write = console_write,
	.put_char = console_put_char,
};

void switch_fg_console(int i)
{
	early_assert(0 <= i && i < NUM_CONSOLE);

	if (i == fg_console)
		return;

	struct console *con = &console_table[i];
	fg_console = i;

	memcpy(VIDEO_MEM, con->mem, N_ROW * N_COL * sizeof(uint16_t));
	move_cursor(con);
}

void console_init()
{
	struct console *con;

	for (int i = 0; i < NUM_CONSOLE; ++i) {
		struct tty_struct *tty = &console_tty_table[i];
		con = &console_table[i];

		con->k.v_flags = 0;
		con->k.v_key = 0;

		con->bg_color = Black;
		con->fg_color = Light_Gray;
		con->pos_x = 0;
		con->pos_y = 0;
		con->save_x = 0;
		con->save_y = 0;
		memset(con->mem, 0, N_ROW * N_COL);

		memset(con->esc_buf, 0, ESC_BUF_SIZE);
		con->esc_buf_p = 0;

		tty->dev = MKDEV(TTY_MAJOR, TTY_MINOR_1 + i);
		tty->driver = &console_driver;
		tty->pgrp = 0;
		tty->session = 0;
		ring_buffer_init(&tty->read_buf);

		INIT_LOCK(&tty->lock);
		init_wait_queue(&tty->wait_read);

		tty->termios = default_termios;
	}

	fg_console = 0;
	con = get_fg_console();
	memcpy(con->mem, VIDEO_MEM, N_ROW * N_COL * sizeof(uint16_t));
	get_cursor(&con->pos_y, &con->pos_x);

	register_tty_driver(&console_driver);
}
