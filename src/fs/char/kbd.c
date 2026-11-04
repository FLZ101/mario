#include <misc.h>
#include <trap.h>
#include <io.h>

#include <fs/tty/tty.h>
#include <fs/tty/console.h>
#include <fs/gui.h>
#include <fs/psaux.h>

#define PS2_WAIT_LIMIT 100000

static int wait_input_empty(void)
{
	unsigned int index;

	for (index = 0; index < PS2_WAIT_LIMIT; index++)
		if (!(inb(0x64) & 0x02))
			return 0;
	return -ETIMEDOUT;
}

static int wait_output_full(unsigned char *value)
{
	unsigned int index;

	for (index = 0; index < PS2_WAIT_LIMIT; index++)
		if (inb(0x64) & 0x01) {
			*value = inb(0x60);
			return 0;
		}
	return -ETIMEDOUT;
}

static int ps2_command(unsigned char command)
{
	if (wait_input_empty())
		return -ETIMEDOUT;
	outb(0x64, command);
	return 0;
}

static int ps2_data(unsigned char value)
{
	if (wait_input_empty())
		return -ETIMEDOUT;
	outb(0x60, value);
	return 0;
}

static int ps2_read(unsigned char *value)
{
	return wait_output_full(value);
}

static int ps2_write_config(unsigned char config)
{
	int error = ps2_command(0x60);

	if (error)
		return error;
	return ps2_data(config);
}

static int ps2_aux_command(unsigned char command)
{
	unsigned int index;
	unsigned char status;
	unsigned char response;

	if (ps2_command(0xd4) || ps2_data(command))
		return -ETIMEDOUT;
	for (index = 0; index < PS2_WAIT_LIMIT; index++) {
		status = inb(0x64);
		if (!(status & 0x01))
			continue;
		response = inb(0x60);
		if (status & 0x20)
			return response == 0xfa ? 0 : -EIO;
	}
	return -ETIMEDOUT;
}

static void ps2_flush_output(void)
{
	unsigned int index;

	for (index = 0; index < PS2_WAIT_LIMIT && (inb(0x64) & 0x01); index++)
		inb(0x60);
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
	0, sym('\\', '|'), 0, 0,
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

static void handle_cursor_key(struct tty_struct *tty, struct console *con, unsigned int __k)
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
	case HOME:
		c = 'H';
		break;
	case END:
		c = 'F';
		break;
	default:
		return;
	}
	tty_receive_c(tty, '\033');
	tty_receive_c(tty, con->k.cursor_app ? 'O' : '[');
	tty_receive_c(tty, c);
}

static uint16_t gui_scancode(unsigned int scan, int extended)
{
	/* The PS/2 controller supplies Set-2 bytes; the GUI ABI fixes Set-1. */
	static const uint8_t set2_to_set1[132] = {
		[0x01] = 0x43, [0x03] = 0x3f, [0x04] = 0x3d, [0x05] = 0x3b,
		[0x06] = 0x3c, [0x07] = 0x58, [0x09] = 0x44, [0x0a] = 0x42,
		[0x0b] = 0x40, [0x0c] = 0x3e, [0x0d] = 0x0f, [0x0e] = 0x29,
		[0x11] = 0x38, [0x12] = 0x2a, [0x14] = 0x1d, [0x15] = 0x10,
		[0x16] = 0x02, [0x1a] = 0x2c, [0x1b] = 0x1f, [0x1c] = 0x1e,
		[0x1d] = 0x11, [0x1e] = 0x03, [0x21] = 0x2e, [0x22] = 0x2d,
		[0x23] = 0x20, [0x24] = 0x12, [0x25] = 0x05, [0x26] = 0x04,
		[0x29] = 0x39, [0x2a] = 0x2f, [0x2b] = 0x21, [0x2c] = 0x14,
		[0x2d] = 0x13, [0x2e] = 0x06, [0x31] = 0x31, [0x32] = 0x30,
		[0x33] = 0x23, [0x34] = 0x22, [0x35] = 0x15, [0x36] = 0x07,
		[0x3a] = 0x32, [0x3b] = 0x24, [0x3c] = 0x16, [0x3d] = 0x08,
		[0x3e] = 0x09, [0x41] = 0x33, [0x42] = 0x25, [0x43] = 0x17,
		[0x44] = 0x18, [0x45] = 0x0b, [0x46] = 0x0a, [0x49] = 0x34,
		[0x4a] = 0x35, [0x4b] = 0x26, [0x4c] = 0x27, [0x4d] = 0x19,
		[0x4e] = 0x0c, [0x52] = 0x28, [0x54] = 0x1a, [0x55] = 0x0d,
		[0x58] = 0x3a, [0x59] = 0x36, [0x5a] = 0x1c, [0x5b] = 0x1b,
		[0x5d] = 0x2b, [0x66] = 0x0e, [0x69] = 0x4f, [0x6b] = 0x4b,
		[0x6c] = 0x47, [0x70] = 0x52, [0x71] = 0x53, [0x72] = 0x50,
		[0x73] = 0x4c, [0x74] = 0x4d, [0x75] = 0x48, [0x76] = 0x01,
		[0x77] = 0x45, [0x78] = 0x57, [0x79] = 0x4e, [0x7a] = 0x51,
		[0x7b] = 0x4a, [0x7c] = 0x37, [0x7d] = 0x49, [0x83] = 0x41,
	};

	if (scan >= sizeof(set2_to_set1) || !set2_to_set1[scan])
		return 0;
	return set2_to_set1[scan] | (extended ? 0x100 : 0);
}

void handle_key(struct tty_struct *tty, struct console *con,
		unsigned int scan, int extended)
{
	unsigned int __i;
	unsigned int __k;

	struct kbd *k = &con->k;

	/*
	 * Function keys stay kernel-owned in graphics mode.  Other keys become
	 * stable Set-1 GUI records and never reach the saved text VT.
	 */
	if (graphics_console_active()) {
		/* The legacy table also labels cursor keys and Enter as FUN(). */
		if (!(k->v_key & (1 << f_0)) || (k->v_key & 0xff) > F12) {
			uint16_t scancode = gui_scancode(scan, extended);

			if (scancode)
				gui_receive_key(scancode, !!(k->v_flags & 0x0100));
			return;
		}
		if (!(k->v_flags & 0x0100))
			return;
		switch (k->v_key & 0xff) {
		case F1:
			switch_fg_console(0);
			break;
		case F2:
			switch_fg_console(1);
			break;
		case F3:
			switch_fg_console(2);
			break;
		case F4:
			switch_fg_console(3);
			break;
		case F5:
			switch_fg_console(4);
			break;
		case F6:
			switch_fg_console(5);
			break;
		case F8:
			enter_graphics_console();
			break;
		}
		return;
	}

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
				if ((k->v_key & (1 << s_hift)) && ((k->v_flags & LSHL) || (k->v_flags & RSHL)))
					tmp *= -1;
				if ((k->v_key & (1 << c_ap)) && (k->v_flags & CAPLOCK))
					tmp *= -1;
				if (tmp == 1)
					__k = k->v_key >> 16;
			}

			int ctl = k->v_flags & (LCTL | RCTL);
			int alt = k->v_flags & (LALT | RALT);
			int fn = __k & (1 << f_0);
			uint8_t ch = __k & 0xff;

			if (ctl) {
				if ('a' <= ch && ch <= 'z') {
					tty_receive_c(tty, ch - 'a' + 1);
				} else {
					switch (ch) {
					case '\\':
						tty_receive_c(tty, 034);
						break;
					case ']':
						tty_receive_c(tty, 035);
						break;
					case '`':
						tty_receive_c(tty, 036);
						break;
					case '/':
						tty_receive_c(tty, 037);
						break;
					}
				}
			}

			if (ctl || alt)
				return;

			if (fn) {
				switch (ch) {
				case ENTER:
					tty_receive_c(tty, '\r');
					break;
				case SPACE:
					tty_receive_c(tty, ' ');
					break;
				case TAB:
					tty_receive_c(tty, '\t');
					break;
				case BACKSPACE:
					tty_receive_c(tty, 0x7f);
					break;
				case ESC:
					tty_receive_c(tty, 033);
					break;
				case LEFT:
				case RIGHT:
				case UP:
				case DOWN:
				case HOME:
				case END:
					handle_cursor_key(tty, con, ch);
					break;
				case DELETE:
					tty_receive_s(tty, "\033[3~");
					break;
				case INSERT:
					tty_receive_s(tty, "\033[2~");
					break;
				case PGDOWN:
					tty_receive_s(tty, "\033[6~");
					break;
				case PGUP:
					tty_receive_s(tty, "\033[5~");
					break;
				case F1:
					switch_fg_console(0);
					break;
				case F2:
					switch_fg_console(1);
					break;
				case F3:
					switch_fg_console(2);
					break;
				case F4:
					switch_fg_console(3);
					break;
				case F5:
					switch_fg_console(4);
					break;
				case F6:
					switch_fg_console(5);
					break;
				case F8:
					/* F8 is reserved for the framebuffer-backed graphics seat. */
					enter_graphics_console();
					break;
				}
				return;
			}

			tty_receive_c(tty, ch);
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
			handle_key(tty, con, __b, 0);
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
			handle_key(tty, con, __b, 1);
		}
		state = 0;
		break;

	case 2:
		__i = code_table[__b];
		if (__i != 0) {
			k->v_key    = __i;
			k->v_flags &= (~0x0100);	/* released */
			handle_key(tty, con, __b, 0);
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
			handle_key(tty, con, __b, 1);
		}
		state = 0;
		break;
	default:
		state++;
	}
}

void ps2_init(void)
{
	unsigned char config;
	unsigned char result;
	int have_config = 0;

	/* Every controller wait is bounded so a missing mouse cannot stall boot. */
	if (ps2_command(0xad) || ps2_command(0xa7))
		goto failed;
	ps2_flush_output();
	if (ps2_command(0x20) || ps2_read(&config))
		goto failed;
	have_config = 1;
	/* Disable IRQs, enable both clocks, and keep the keyboard in Set-2 mode. */
	config &= ~(0x73);
	if (ps2_write_config(config) || ps2_command(0xae))
		goto failed;
	config |= 0x01;
	if (ps2_write_config(config))
		goto failed;
	if (ps2_command(0xa9) || ps2_read(&result) || result)
		goto failed;
	if (ps2_command(0xa8) || ps2_aux_command(0xf6) ||
	    ps2_aux_command(0xf4))
		goto failed;
	/* Enable IRQ12 only after defaults and streaming mode were acknowledged. */
	config |= 0x02;
	if (ps2_write_config(config))
		goto failed;
	psaux_controller_ready();
	return;

failed:
	ps2_command(0xa7);
	/* An auxiliary failure must leave the existing keyboard usable. */
	ps2_command(0xae);
	if (have_config) {
		config &= ~(0x72);
		config |= 0x01;
		ps2_write_config(config);
	}
	psaux_controller_failed();
}
