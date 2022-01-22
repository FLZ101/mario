#include <fs/tty/console.h>
#include <fs/tty/tty.h>

#include <lib/string.h>
#include <lib/stddef.h>

#include <misc.h>
#include <io.h>

#define TEXT (0xb8000 + KERNEL_BASE)

#define COLOR 0x07

#define MAKEC(c) (COLOR << 8 | (c))

#define SPACE MAKEC(' ')

static void move_cursor(struct console *con)
{
	unsigned int pos;

	pos = 80*con->pos_y + con->pos_x;

	outb(0x3d4, 14);
	outb(0x3d5, pos >> 8);
	outb(0x3d4, 15);
	outb(0x3d5, pos);
}

static void set_pos(struct console *con, int x, int y)
{
	if (0 <= x && x < 25)
		con->pos_x = x;
	if (0 <= y && y < 80)
		con->pos_y = y;

	move_cursor(con);
}

static void reset_console(struct console *con)
{
	memsetw((void *)TEXT, SPACE, 80*25);
	set_pos(con, 0, 0);
}

extern int pos_x, pos_y;
void console_init(struct console *con)
{
	con->pos_x = pos_x;
	con->pos_y = pos_y;
	con->save_x = con->save_y = 0;
}

extern struct tty_struct *tty_one;
void west(void)
{
	set_pos(&tty_one->con, pos_x, pos_y);
}

static void scroll_one_line(void)
{
	memmove((void *)TEXT, (void *)(TEXT + 80*2), 80*2*24);
	memsetw((void *)(TEXT + 80*2*24), SPACE, 80);
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
		*((short *)TEXT + 80*con->pos_y + con->pos_x) = SPACE;
	} else if (c >= ' ') {
		*((short *)TEXT + 80*con->pos_y + con->pos_x) = MAKEC(c);
		con->pos_x++;
	}

	if (con->pos_x >= 80) {
		con->pos_x = 0;
		con->pos_y++;
	}
	if (con->pos_y >= 25) {
		con->pos_y = 24;
		scroll_one_line();
	}
	move_cursor(con);
}

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

#define BUF_SIZE 6
static char buf[BUF_SIZE] = {'\0'};
static int buf_p = 0;

static int into_buf(unsigned char c)
{
	buf[buf_p++] = c;

	if (buf_p == BUF_SIZE-1)
		return 1;
	return 0;
}

static void reset_buf(void)
{
	memset(buf, 0, BUF_SIZE);
	buf_p = 0;
}

static char *get_arg(char *str)
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

static void csi_H(struct console *con, unsigned char c)
{
	char *tmp;
	unsigned int pos_x, pos_y;

	pos_x = pos_y = 0;

	tmp = get_arg(buf);
	if (!tmp)
		goto __set_pos;
	pos_x = simple_atou(tmp);
	if (pos_x == (unsigned int)-1)
		return;
	tmp = get_arg(NULL);
	if (!tmp)
		goto __set_pos;
	pos_y = simple_atou(tmp);
	if (pos_y == (unsigned int)-1)
		return;
	if (get_arg(NULL))	/* Is there more arg? */
		return;
__set_pos:
	set_pos(con, pos_x, pos_y);
}

static void csi_ABCD(struct console *con, unsigned char c)
{
	char *tmp;
	unsigned int count;

	count = 1;

	tmp = get_arg(buf);
	if (!tmp)
		goto __move;
	count = simple_atou(tmp);
	if (count == (unsigned int)-1)
		return;
	if (get_arg(NULL))
		return;
__move:
	switch (c) {
	case 'A':
		con->pos_y -= count;
		if (con->pos_y > 24)
			con->pos_y = 0;
		break;
	case 'B':
		con->pos_y += count;
		if (con->pos_y > 24)
			con->pos_y = 24;
		break;
	case 'C':
		con->pos_x += count;
		if (con->pos_x > 79)
			con->pos_x = 79;
		break;
	case 'D':
		con->pos_x -= count;
		if (con->pos_x > 79)
			con->pos_x = 0;
		break;
	default:
		;
	}
	move_cursor(con);
}

static void csi_s(struct console *con, unsigned char c)
{
	if (get_arg(buf))
		return;
	con->save_x = con->pos_x;
	con->save_y = con->pos_y;
}

static void csi_u(struct console *con, unsigned char c)
{
	if (get_arg(buf))
		return;
	con->pos_x = con->save_x;
	con->pos_y = con->save_y;
	move_cursor(con);
}

static void csi(struct console *con, unsigned char c)
{
	if (c == 'H')
		csi_H(con, c);
	else if (c == 'A' || c == 'B' || c == 'C' || c == 'D')
		csi_ABCD(con, c);
	else if (c == 's')
		csi_s(con, c);
	else if (c == 'u')
		csi_u(con, c);
}

void console_write(struct console *con, unsigned char c)
{
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
		break;
	case ESC:
		if (c == '[') {
			reset_buf();
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
		if (into_buf(c))
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
