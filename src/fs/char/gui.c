#include <fs/chrdev.h>
#include <fs/fs.h>
#include <fs/gui.h>
#include <fs/tty/console.h>
#include <lib/spinlock.h>
#include <video/fbcon.h>

/* Preserve ownership when fork() copies a file description. */
#define FMODE_GUI_OWNER 0x400

static spinlock_t gui_lock = SPINLOCK_UNLOCKED;
static unsigned int gui_owner_refs;
static unsigned int fb_writer_refs;

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
	if (!gui_owner_refs && fb_writer_refs)
		lost_seat = 1;
	spin_unlock_irqrestore(&gui_lock, flags);
	if (lost_seat)
		switch_text_console(get_fg_text_console_index());
}

static struct file_operations gui_fops = {
	.open = gui_open,
	.release = gui_release,
};

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
	if (!fb_writer_refs && gui_owner_refs)
		lost_seat = 1;
	spin_unlock_irqrestore(&gui_lock, flags);
	if (lost_seat)
		switch_text_console(get_fg_text_console_index());
}

void gui_init(void)
{
	register_chrdev(GUI_MAJOR, &gui_fops);
}
