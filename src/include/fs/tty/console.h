#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <misc.h>

struct kbd {
	unsigned int v_flags;
	unsigned int v_key;
};

#define N_COL 80
#define N_ROW 25

#define ESC_BUF_SIZE 6

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

	uint16_t mem[N_ROW][N_COL];
};

#define VIDEO_MEM ((uint16_t (*)[N_COL])(0xb8000 + KERNEL_BASE))

struct tty_struct *get_fg_tty();

struct console *get_fg_console();

void switch_fg_console(int i);

#endif /* _CONSOLE_H */
