#ifndef _TTY_H
#define _TTY_H

#include <fs/tty/console.h>
#include <fs/tty/kbd.h>

#include <sched.h>

struct tty_struct {
	struct console con;
	struct kbd k;
	char *buf;
	int head, tail;
	wait_queue_t wait;
};

#endif	/* _TTY_H */
