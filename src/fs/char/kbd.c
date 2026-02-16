#include <misc.h>
#include <trap.h>
#include <io.h>

#include <fs/tty/tty.h>
#include <fs/tty/console.h>

static void wait_input_empty(void)
{
	char c;

	do {
		c = inb(0x64);
	} while (c & 0x02);
}

static void wait_output_full(void)
{
	char c;

	do {
		c = inb(0x64);
	} while (!(c & 0x01));
}

/* Function Keys */
#define F1 0
#define F2 1
#define F3 2
#define F4 3
#define F5 4
#define F6 5
#define F7 6
#define F8 7
#define F9 8
#define F10 9
#define F11 10
#define F12 11
#define ESC 12
#define SPACE 13
#define ENTER 14
#define TAB 15
#define BACKSPACE 16
#define DELETE 17
#define INSERT 18
#define HOME 19
#define END 20
#define PGDOWN 21
#define PGUP 22
#define UP 23
#define DOWN 24
#define LEFT 25
#define RIGHT 26

char *fun_str[] = {
	"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
	"ESC", "SPACE", "ENTER", "TAB", "BACKSPACE", "DELETE", "INSERT", "HOME", "END", "PGDOWN",
	"PGUP", "UP", "DOWN", "LEFT", "RIGHT"
};

/* Control Keys */
#define LSHL (1 << 0)
#define RSHL (1 << 1)
#define LALT (1 << 2)
#define RALT (1 << 3)
#define LCTL (1 << 4)
#define RCTL (1 << 5)
#define NUMLOCK (1 << 6)
#define CAPLOCK (1 << 7)


/*
 * B3B2B1B0
 * B1 x|x|x|s_hift|c_ap|n_um|c_trl|f_0
 *
 * f_0		: Function Key?
 * c_trl	: Control Key?
 *
 * n_m		: NUMLOCK toggleable?
 * c_ap		: CAPLOCK toggleable?
 * s_hift	: SHIFT toggleable (and B3B2 is the other value)?
 */

#define s_hift 12
#define c_ap 11
#define n_um 10
#define c_trl 9
#define f_0 8

#define FUN(a) (a)|(1 << f_0) 								//功能键
#define CTL(a) (a)|(1 << c_trl)								//控制键

#define alp(a, b) (a)|(1 << c_ap)|(1 << s_hift)|((b) << 16) //字母
#define sym(a, b) (a)|(1 << s_hift)|(b << 16)				//符号
#define kpd(a, b) (a)|(1 << s_hift)|(1 << n_um)|((b) << 16) //小键盘

/*
 * Scan Code Set 2 (US QWERTY)
 */
int code_table[132] = {
	0, FUN(F9), 0, FUN(F5),
	FUN(F3), FUN(F1), FUN(F2), FUN(F12),
	0, FUN(F10), FUN(F8), FUN(F6),
	FUN(F4), FUN(TAB), alp('`', '~'), 0,
	0, CTL(LALT), CTL(LSHL), 0,
	CTL(LCTL), alp('q', 'Q'), sym('1', '!'), 0,
	0, 0, alp('z', 'Z'), alp('s', 'S'),
	alp('a', 'A'), alp('w', 'W'), sym('2', '@'), 0,
	0, alp('c', 'C'), alp('x', 'X'), alp('d', 'D'),
	alp('e', 'E'), sym('4', '$'), sym('3', '#'), 0,
	0, FUN(SPACE), alp('v', 'V'), alp('f', 'F'),
	alp('t', 'T'), alp('r', 'R'), sym('5', '%'), 0,
	0, alp('n', 'N'), alp('b', 'B'), alp('h', 'H'),
	alp('g', 'G'), alp('y', 'Y'), sym('6', '^'), 0,
	0, 0, alp('m', 'M'), alp('j', 'J'),
	alp('u', 'U'), sym('7', '&'), sym('8', '*'), 0,
	0, sym(',', '<'), alp('k', 'K'), alp('i', 'I'),
	alp('o', 'O'), sym('0', ')'), sym('9', '('), 0,
	0, sym('.', '>'), sym('/', '?'), alp('l', 'L'),
	sym(';', ':'), alp('p', 'P'), sym('-', '_'), 0,
	0, 0, sym('\'', '"'), 0,
	sym('[', '{'), sym('=', '+'), 0, 0,
	CTL(CAPLOCK), CTL(RSHL), FUN(ENTER), sym(']', '}'),
	0, '\\', 0, 0,
	0, 0, 0, 0,
	0, 0, FUN(BACKSPACE), 0,
	0, kpd(FUN(END), '1'), 0, kpd(FUN(LEFT), '4'),
	kpd(FUN(HOME), '7'), 0, 0, 0,
	kpd(FUN(INSERT), '0'), kpd(FUN(DELETE), '.'), kpd(FUN(DOWN), '2'), kpd('5', '5'),
	kpd(FUN(RIGHT), '6'), kpd(FUN(UP), '8'), FUN(ESC), CTL(NUMLOCK),
	FUN(F11), kpd('+', '+'), kpd(FUN(PGDOWN), '3'), kpd('-', '-'),
	kpd('*', '*'), kpd(FUN(PGUP), '9'), 0, 0,
	0, 0, 0, FUN(F7)
};

struct k_e0 {
	unsigned int code;
	unsigned int key;
};

struct k_e0 code_e0[15] = {
	{0x11, FUN(RALT)}, {0x14, FUN(RCTL)}, {0x4a, kpd('/', '/')},
	{0x5a, kpd(FUN(ENTER), FUN(ENTER))}, {0x69, FUN(END)}, {0x6b, FUN(LEFT)},
	{0x6c, FUN(HOME)}, {0x70, FUN(INSERT)}, {0x71, FUN(DELETE)}, {0x72, FUN(DOWN)},
	{0x74, FUN(RIGHT)}, {0x75, FUN(UP)}, {0x7a, FUN(PGDOWN)}, {0x7d, FUN(PGUP)}
};

unsigned int get_e0_key(unsigned int code)
{
	int i;
	for (i = 0; i < 15; i++) {
		if (code_e0[i].code == code)
			return code_e0[i].key;
	}
	return 0;
}

void csi(struct tty_struct *tty, unsigned int __k)
{
	char c;

	switch (__k) {
	case UP:
		c = 'A';
		break;
	case DOWN:
		c = 'B';
		break;
	case RIGHT:
		c = 'C';
		break;
	case LEFT:
		c = 'D';
		break;
	default:
		return;
	}
	tty_receive_c(tty, '\033');
	tty_receive_c(tty, '[');
	tty_receive_c(tty, c);
}

void handle_key(struct tty_struct *tty, struct console *con)
{
	unsigned int __i;
	unsigned int __k;

	struct kbd *k = &con->k;

	if ((k->v_flags & 0x0100)) {			/* pressed */
		if (k->v_key & (1 << c_trl)) {
			__i = k->v_key & 0xff;

			if ((__i == NUMLOCK) || (__i == CAPLOCK))
				k->v_flags ^= __i;
			else
				k->v_flags |= __i;
		} else {
			__k = k->v_key & 0xffff;

			if (k->v_key & (1 << n_um)) {	/* keypad */
				if ((k->v_flags & NUMLOCK) && (!(k->v_flags & RSHL)) && (!(k->v_flags & LSHL)))
					__k = k->v_key >> 16;

			} else {
				int tmp = -1;
				if ((k->v_key & (1 << s_hift)) && \
						((k->v_flags & LSHL) || (k->v_flags & RSHL)))
					tmp *= -1;
				if ((k->v_key & (1 << c_ap)) && \
						(k->v_flags & CAPLOCK))
					tmp *= -1;
				if (tmp == 1)
					__k = k->v_key >> 16;
			}
			if (k->v_flags & 0x3c) {
				if (k->v_flags & (LALT | RALT))
					;

				if (k->v_flags & (LCTL | RCTL)) {
					uint8_t ch = __k & 0xff;
					if ('@' <= ch && ch <= '_') {
						tty_receive_c(tty, ch - '@');
					} else if ('a' <= ch && ch <= 'z') {
						tty_receive_c(tty, ch - 'a' + 1);
					} else if (ch == '?') {
						tty_receive_c(tty, 0x7f); // DEL
					}
				}
				return;
			}
			if (__k & (1 << f_0)) {
				__k &= 0xff;

				if (__k == ENTER)
					tty_receive_c(tty, '\r');
				else if (__k == SPACE)
					tty_receive_c(tty, ' ');
				else if (__k == TAB)
					tty_receive_c(tty, '\t');
				else if (__k == BACKSPACE)
					tty_receive_c(tty, 0x7f);
				else if (__k == LEFT || __k == RIGHT || __k == UP || __k == DOWN)
					csi(tty, __k);
				else if (__k == ESC)
					tty_receive_c(tty, 033);
				else if (__k == DELETE)
					tty_receive_s(tty, "\033[3~");
				else if (__k == F1)
					switch_fg_console(0);
				else if (__k == F2)
					switch_fg_console(1);
				else if (__k == F3)
					switch_fg_console(2);
				else if (__k == F4)
					switch_fg_console(3);
				else if (__k == F5)
					switch_fg_console(4);
				else if (__k == F6)
					switch_fg_console(5);
				else
					/* Do nothing */;
			} else {
				tty_receive_c(tty, __k & 0xff);
			}
		}
	} else if (k->v_key & (1 << c_trl)) { 	/* released */
		__i = k->v_key & 0xff;
		if ((__i != NUMLOCK) && (__i != CAPLOCK))
			k->v_flags &= (~__i);
	}
}

#define PAUSE	-7
#define PRINT_P	-2
#define PRINT_R	-3

void irq_PS2(void)
{
	static int state = 0;

	unsigned int __i;
	unsigned int __b;

	struct tty_struct *tty = get_fg_tty();
	struct console *con = get_fg_console();
	struct kbd *k = &con->k;

	__b = inb(0x60);

	switch (state) {
	case 0:
		if (__b == 0xe1) {
			state = PAUSE;
			break;
		}
		if (__b == 0xe0) {
			state = 1;
			break;
		}
		if (__b == 0xf0) {
			state = 2;
			break;
		}
		__i = code_table[__b];
		if (__i != 0) {
			k->v_key    = __i;
			k->v_flags |= 0x0100; /* pressed */
			handle_key(tty, con);
		}
		break;

	case 1:
		if (__b == 0x12) {
			state = PRINT_P;
			break;
		}
		if (__b == 0xf0) {
			state = 3;
			break;
		}
		__i = get_e0_key(__b);
		if (__i != 0) {
			k->v_key    = __i;
			k->v_flags |= 0x0100;	/* pressed */
			handle_key(tty, con);
		}
		state = 0;
		break;

	case 2:
		__i = code_table[__b];
		if (__i != 0) {
			k->v_key    = __i;
			k->v_flags &= (~0x0100);	/* released */
			handle_key(tty, con);
		}
		state = 0;
		break;

	case 3:
		if (__b == 0x7c) {
			state = PRINT_R;
			break;
		}
		__i = get_e0_key(__b);
		if (__i != 0) {
			k->v_key    = __i;
			k->v_flags &= (~0x0100);	/* released */
			handle_key(tty, con);
		}
		state = 0;
		break;
	default:
		state++;
	}
}

void __tinit ps2_init(void)
{
	unsigned char c;

	wait_input_empty();
	outb(0x64, 0xad);	/* disable port 1 (keyboard) */
	wait_input_empty();
	outb(0x64, 0xa7);	/* disable port 2 (mouse) */
	wait_input_empty();
	outb(0x64, 0x20);
	wait_output_full();
	c = 0xbf & inb(0x60);
	wait_input_empty();
	outb(0x64, 0x60);
	wait_input_empty();
	outb(0x60, c);		/* disable the first PS/2 port translation */
	wait_input_empty();
	outb(0x64, 0xae);
}
