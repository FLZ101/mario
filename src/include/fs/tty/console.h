#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <types.h>
#include <misc.h>

struct kbd {
	unsigned int v_flags;
	unsigned int v_key;
};

#define N_COL 80
#define N_ROW 25

#define ESC_BUF_SIZE 6

enum esc_state {
	NORMAL,
	ESC,
	CSI,
	BAD
};

struct console {
	struct kbd k;

	// +-----> x
	// |
	// |
	// y
	unsigned int pos_x, pos_y;
	unsigned int save_x, save_y;
	uint8_t fg_color, bg_color;

	char esc_buf[ESC_BUF_SIZE];
	int esc_buf_p;
	enum esc_state state;

	uint16_t mem[N_ROW][N_COL];
};

struct tty_struct *get_fg_tty();

struct console *get_fg_console();

void switch_fg_console(int i);

#endif /* _CONSOLE_H */