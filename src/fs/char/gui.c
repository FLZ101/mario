#include <fs/chrdev.h>
#include <fs/fs.h>
#include <fs/gui.h>
#include <fs/tty/console.h>
#include <lib/spinlock.h>
#include <time.h>
#include <uapi/mario/input.h>
#include <video/fbcon.h>

/* Preserve ownership when fork() copies a file description. */
#define FMODE_GUI_OWNER 0x400
#define GUI_EVENT_COUNT 256

static spinlock_t gui_lock = SPINLOCK_UNLOCKED;
static wait_queue_t gui_wait;
static unsigned int gui_owner_refs;
static unsigned int fb_writer_refs;
static struct mario_gui_key_event gui_events[GUI_EVENT_COUNT];
static unsigned int gui_event_head;
static unsigned int gui_event_tail;
static unsigned int gui_event_count;

static int gui_read(struct inode *i, struct file *f, char *buf, int count);
static int gui_poll(struct file *f, struct poll_context *ctx);

static void gui_clear_events(void)
{
	gui_event_head = 0;
	gui_event_tail = 0;
	gui_event_count = 0;
}

static int gui_open(struct inode *i, struct file *f)
{
	unsigned long flags;

	(void)i;
	if (!fbcon_enabled())
		return -ENODEV;
	if (f->f_mode & FMODE_GUI_OWNER) {
		spin_lock_irqsave(&gui_lock, flags);
		gui_owner_refs++;
		spin_unlock_irqrestore(&gui_lock, flags);
		return 0;
	}
	if (f->f_mode & 2)
		return -EACCES;
	spin_lock_irqsave(&gui_lock, flags);
	if (gui_owner_refs) {
		spin_unlock_irqrestore(&gui_lock, flags);
		return -EBUSY;
	}
	gui_owner_refs = 1;
	gui_clear_events();
	f->f_mode |= FMODE_GUI_OWNER;
	spin_unlock_irqrestore(&gui_lock, flags);
	return 0;
}

static void gui_release(struct inode *i, struct file *f)
{
	unsigned long flags;
	int lost_seat = 0;

	(void)i;
	if (!(f->f_mode & FMODE_GUI_OWNER))
		return;
	spin_lock_irqsave(&gui_lock, flags);
	assert(gui_owner_refs);
	gui_owner_refs--;
	if (!gui_owner_refs) {
		gui_clear_events();
		if (fb_writer_refs)
			lost_seat = 1;
	}
	spin_unlock_irqrestore(&gui_lock, flags);
	if (lost_seat)
		switch_text_console(get_fg_text_console_index());
}

static struct file_operations gui_fops = {
	.open = gui_open,
	.release = gui_release,
	.read = gui_read,
	.poll = gui_poll,
};

static int gui_read(struct inode *i, struct file *f, char *buf, int count)
{
	unsigned long flags;
	unsigned int events;
	int bytes;

	(void)i;
	if (!count)
		return 0;
	if (count < sizeof(gui_events[0]) || count % sizeof(gui_events[0]))
		return -EINVAL;
	for (;;) {
		spin_lock_irqsave(&gui_lock, flags);
		if (gui_event_count) {
			events = count / sizeof(gui_events[0]);
			if (events > gui_event_count)
				events = gui_event_count;
			bytes = events * sizeof(gui_events[0]);
			if (verify_area(VERIFY_WRITE, buf, bytes)) {
				spin_unlock_irqrestore(&gui_lock, flags);
				return -EFAULT;
			}
			for (unsigned int index = 0; index < events; index++) {
				memcpy_tofs(buf + index * sizeof(gui_events[0]),
					     &gui_events[gui_event_tail],
					     sizeof(gui_events[0]));
				gui_event_tail = (gui_event_tail + 1) % GUI_EVENT_COUNT;
			}
			gui_event_count -= events;
			spin_unlock_irqrestore(&gui_lock, flags);
			return bytes;
		}
		spin_unlock_irqrestore(&gui_lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		sleep_on(&gui_wait, TASK_INTERRUPTIBLE, NULL);
	}
}

static int gui_poll(struct file *f, struct poll_context *ctx)
{
	unsigned long flags;
	int mask = 0;

	poll_wait(f, &gui_wait, ctx);
	spin_lock_irqsave(&gui_lock, flags);
	if (gui_event_count)
		mask = POLLIN;
	spin_unlock_irqrestore(&gui_lock, flags);
	return mask;
}

int gui_seat_ready(void)
{
	unsigned long flags;
	int ready;

	spin_lock_irqsave(&gui_lock, flags);
	ready = gui_owner_refs && fb_writer_refs;
	spin_unlock_irqrestore(&gui_lock, flags);
	return ready;
}

void gui_fb_writer_open(void)
{
	unsigned long flags;

	spin_lock_irqsave(&gui_lock, flags);
	fb_writer_refs++;
	spin_unlock_irqrestore(&gui_lock, flags);
}

void gui_fb_writer_release(void)
{
	unsigned long flags;
	int lost_seat = 0;

	spin_lock_irqsave(&gui_lock, flags);
	assert(fb_writer_refs);
	fb_writer_refs--;
	if (!fb_writer_refs) {
		gui_clear_events();
		if (gui_owner_refs)
			lost_seat = 1;
	}
	spin_unlock_irqrestore(&gui_lock, flags);
	if (lost_seat)
		switch_text_console(get_fg_text_console_index());
}

void gui_receive_key(uint16_t scancode, int pressed)
{
	struct mario_gui_key_event event = {
		.scancode = scancode,
		.pressed = pressed,
		.ticks = jiffies,
	};
	unsigned long flags;

	spin_lock_irqsave(&gui_lock, flags);
	if (!gui_owner_refs || !fb_writer_refs) {
		spin_unlock_irqrestore(&gui_lock, flags);
		return;
	}
	/* Never stall the IRQ path: retain the newest event and mark the loss. */
	if (gui_event_count == GUI_EVENT_COUNT) {
		gui_event_tail = (gui_event_tail + 1) % GUI_EVENT_COUNT;
		gui_event_count--;
		event.flags = GUI_KEY_EVENT_OVERFLOW;
	}
	gui_events[gui_event_head] = event;
	gui_event_head = (gui_event_head + 1) % GUI_EVENT_COUNT;
	gui_event_count++;
	spin_unlock_irqrestore(&gui_lock, flags);
	wake_up_interruptible(&gui_wait);
}

void gui_init(void)
{
	init_wait_queue(&gui_wait);
	register_chrdev(GUI_MAJOR, &gui_fops);
}
