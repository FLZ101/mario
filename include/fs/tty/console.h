#ifndef _CONSOLE_H
#define _CONSOLE_H

struct console {
	unsigned int pos_x, pos_y;
	unsigned int save_x, save_y;
};

void console_init(struct console *con);

#endif /* _CONSOLE_H */