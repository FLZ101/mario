#include <fs/chrdev.h>
#include <fs/tty/tty.h>

#include <misc.h>

#include <mm/page_alloc.h>

extern void ps2_init(void);

static struct tty_struct tty;
struct tty_struct *tty_one = &tty;

int tty_open(dev_t dev)
{
	console_init(&tty_one->con);
	kbd_init(&tty_one->k);
	tty_one->buf = (char *)page_alloc();
	tty_one->head = 0;
	tty_one->tail = 1;
	init_wait_queue(&tty_one->wait);
	return 0;
}

int ahead(int head)
{
	if (++head == PAGE_SIZE)
		head = 0;
	return head;
}

int tty_read(dev_t dev, char *c)
{
__try:
	if (ahead(tty_one->head) == tty_one->tail) {
		sleep_on_interruptible(&tty_one->wait);
		goto __try;
	}
	tty_one->head = ahead(tty_one->head);
	*c = tty_one->buf[tty_one->head];
	return 0;
}

extern void console_write(struct console *con, unsigned char c);

int tty_write(dev_t dev, char *c)
{
	console_write(&tty_one->con, *c);
	return 0;
}

struct chrdev_operations tty_ops = {
	tty_open,
	tty_read,
	tty_write
};

void __tinit tty_init(void)
{
	ps2_init();
	register_chrdev(TTY_MAJOR, &tty_ops);
}
