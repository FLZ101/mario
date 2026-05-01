#ifndef _CONSOLE_H
#define _CONSOLE_H

#include <types.h>
#include <misc.h>

struct kbd {
	unsigned int v_flags;
	unsigned int v_key;
	int cursor_app;	// cursor application mode
	int keypad_app;	// keypad application mode
};

#define N_COL 80
#define N_ROW 25

#define ESC_BUF_SIZE 64

enum esc_state {
	NORMAL,
	ESC,
	CSI,
	CSI_Q,		// ?
	CSI_X,		// >
	CSI_I,		// !
	CHARSET_G0,	// Designate G0 Character Set, VT100, ISO 2022.
	CHARSET_G1,
	BAD
};

struct console {
	struct kbd k;

	char esc_buf[ESC_BUF_SIZE];
	int esc_buf_p;

	long esc_time;

	enum esc_state state;

	unsigned char charset_G0;
	unsigned char charset_G1;

	unsigned char pgc; // preceding graphic character

	// +-----> x
	// |
	// |
	// y
	unsigned int pos_x, pos_y;
	unsigned int save_x, save_y;
	uint8_t fg_color, bg_color;
	int cursor_hidden;
	int pending_wrap;
	int color_inverted;
	int bold;
	int scr_start, scr_end; // scrolling region

	uint16_t mem[N_ROW][N_COL];

	struct {
		uint16_t mem[N_ROW][N_COL];
		unsigned int pos_x, pos_y;
		unsigned int save_x, save_y;
		int scr_start, scr_end; // scrolling region
	} orig;
};

dev_t get_fg_console_dev();
struct tty_struct *get_fg_tty();
struct console *get_fg_console();
void switch_fg_console(int i);

#define SCREEN_BUF_SIZE (N_ROW * N_COL)
#define SCREEN_BUF_BYTE_SIZE (N_ROW * N_COL * sizeof(uint16_t))
#define SCREEN_ROW_BYTE_SIZE (N_COL * sizeof(uint16_t))

#endif /* _CONSOLE_H */
