#include <fs/fs.h>
#include <fs/fbdev.h>
#include <fs/gui.h>
#include <video/fb.h>

#define FMODE_FB_WRITER 0x800
#define FMODE_FB_OPEN 0x1000

static unsigned int fb_writer_refs;
static unsigned int fb_console_state;
static int fb_console_transitioning;
static unsigned long fb_console_generation;
static spinlock_t fb_lock = SPINLOCK_UNLOCKED;
static wait_queue_t fb_console_wait;
#ifdef CONFIG_FB_TRANSITION_RACE_SELF_TEST
static int fb_transition_race_paused;
#endif

static int fb_read_at(struct inode *i, struct file *f, char *buf, int count,
		      off64_t *position);

/* Keep the state predicate pure so the self-test needs no framebuffer copy. */
static int fb_write_generation_valid(unsigned int state, int transitioning,
				     unsigned long generation,
				     unsigned long snapshot)
{
	return !transitioning && state == FB_CONSOLE_GRAPHICS &&
	       generation == snapshot;
}

static int fb_write_generation_snapshot(unsigned long *snapshot)
{
	unsigned long flags;
	int result = 0;

	spin_lock_irqsave(&fb_lock, flags);
	if (!fb_write_generation_valid(fb_console_state, fb_console_transitioning,
				       fb_console_generation, fb_console_generation))
		result = -EBUSY;
	else
		*snapshot = fb_console_generation;
	spin_unlock_irqrestore(&fb_lock, flags);
	return result;
}

#ifdef CONFIG_FB_WRITE_GENERATION_SELF_TEST
static void fb_write_generation_selftest(void)
{
	assert(fb_write_generation_valid(FB_CONSOLE_GRAPHICS, 0, 7, 7));
	assert(!fb_write_generation_valid(FB_CONSOLE_TEXT, 0, 7, 7));
	assert(!fb_write_generation_valid(FB_CONSOLE_GRAPHICS, 1, 7, 7));
	assert(!fb_write_generation_valid(FB_CONSOLE_GRAPHICS, 0, 8, 7));
}
#endif

static int fb_open(struct inode *i, struct file *f)
{
	unsigned long flags;
	if (!fb_is_enabled())
		return -ENODEV;
	if (f->f_mode & FMODE_FB_OPEN) {
		if (!(f->f_mode & FMODE_FB_WRITER))
			return 0;
		spin_lock_irqsave(&fb_lock, flags);
		fb_writer_refs++;
		spin_unlock_irqrestore(&fb_lock, flags);
		gui_fb_writer_open();
		return 0;
	}
	spin_lock_irqsave(&fb_lock, flags);
	f->private_data = (void *)fb_console_generation;
	f->f_mode |= FMODE_FB_OPEN;
	if (!(f->f_mode & 2)) {
		spin_unlock_irqrestore(&fb_lock, flags);
		return 0;
	}
	if (fb_writer_refs) {
		spin_unlock_irqrestore(&fb_lock, flags);
		return -EBUSY;
	}
	fb_writer_refs = 1;
	f->f_mode |= FMODE_FB_WRITER;
	spin_unlock_irqrestore(&fb_lock, flags);
	gui_fb_writer_open();
	return 0;
}

static void fb_release(struct inode *i, struct file *f)
{
	unsigned long flags;
	if (!(f->f_mode & FMODE_FB_WRITER))
		return;
	spin_lock_irqsave(&fb_lock, flags);
	assert(fb_writer_refs);
	fb_writer_refs--;
	spin_unlock_irqrestore(&fb_lock, flags);
	gui_fb_writer_release();
}

static int fb_lseek(struct inode *i, struct file *f, off_t offset, int origin)
{
	const struct fb_info *info = fb_get_info();
	off64_t pos;
	switch (origin) {
	case SEEK_SET: pos = offset; break;
	case SEEK_CUR: pos = f->f_pos + offset; break;
	case SEEK_END: pos = info->size + offset; break;
	default: return -EINVAL;
	}
	if (pos < 0 || pos > info->size)
		return -EINVAL;
	f->f_pos = pos;
	return pos;
}

static int fb_read(struct inode *i, struct file *f, char *buf, int count)
{
	off64_t position = f->f_pos;
	int result;

	result = fb_read_at(i, f, buf, count, &position);
	if (result > 0)
		f->f_pos = position;
	return result;
}

static int fb_read_at(struct inode *i, struct file *f, char *buf, int count,
		      off64_t *position)
{
	const struct fb_info *info = fb_get_info();
	if (!position || *position < 0 || count < 0)
		return -EINVAL;
	if (*position >= info->size)
		return 0;
	if (count > info->size - *position)
		count = info->size - *position;
	memcpy_tofs(buf, (char *)info->virt_addr + *position, count);
	*position += count;
	return count;
}

static int fb_write_at(struct inode *i, struct file *f, char *buf, int count,
		       off64_t *position)
{
	const struct fb_info *info = fb_get_info();
	unsigned long generation;
	unsigned long flags;
	int completed = 0;
	int remaining;
#ifdef CONFIG_FB_TRANSITION_RACE_SELF_TEST
	int wait_for_transition = 0;
#endif
#ifdef CONFIG_FB_WRITE_CHUNK_SELF_TEST
	int inject_generation_change = 1;
#endif
	if (!position || *position < 0 || count < 0)
		return -EINVAL;
	if (fb_write_generation_snapshot(&generation))
		return -EBUSY;
	spin_lock_irqsave(&fb_lock, flags);
	/* A transition after the snapshot owns scanout before the first byte. */
	if (!fb_write_generation_valid(fb_console_state, fb_console_transitioning,
				       fb_console_generation, generation)) {
		spin_unlock_irqrestore(&fb_lock, flags);
		return -EBUSY;
	}
	if (*position >= info->size) {
		spin_unlock_irqrestore(&fb_lock, flags);
		return count ? -ENOSPC : 0;
	}
	if (count > info->size - *position)
		count = info->size - *position;
	spin_unlock_irqrestore(&fb_lock, flags);
	if (!info->pitch)
		return -EINVAL;
	remaining = count;
	while (remaining) {
		int chunk = info->pitch - *position % info->pitch;

		if (chunk > remaining)
			chunk = remaining;
		spin_lock_irqsave(&fb_lock, flags);
		/* Each scanline starts only while the original frame is current. */
		if (!fb_write_generation_valid(fb_console_state,
					       fb_console_transitioning,
					       fb_console_generation, generation)) {
			spin_unlock_irqrestore(&fb_lock, flags);
			return completed ? completed : -EBUSY;
		}
		memcpy_fromfs((char *)info->virt_addr + *position, buf + completed,
				      chunk);
#ifdef CONFIG_FB_WRITE_CHUNK_SELF_TEST
		/* The rebuilt test kernel invalidates exactly one multi-row write. */
		if (inject_generation_change && remaining > chunk) {
			fb_console_generation++;
			inject_generation_change = 0;
		}
#endif
		spin_unlock_irqrestore(&fb_lock, flags);
		*position += chunk;
		completed += chunk;
		remaining -= chunk;
#ifdef CONFIG_FB_TRANSITION_RACE_SELF_TEST
		/* Pause after one visible row so QMP can switch to text deterministically. */
		spin_lock_irqsave(&fb_lock, flags);
		if (!fb_transition_race_paused && remaining) {
			fb_transition_race_paused = 1;
			wait_for_transition = 1;
		}
		spin_unlock_irqrestore(&fb_lock, flags);
		while (wait_for_transition) {
			spin_lock_irqsave(&fb_lock, flags);
			wait_for_transition = !fb_console_transitioning;
			spin_unlock_irqrestore(&fb_lock, flags);
			if (wait_for_transition)
				safe_halt();
		}
#endif
	}
	return completed;
}

static int fb_write(struct inode *i, struct file *f, char *buf, int count)
{
	off64_t position = f->f_pos;
	int result = fb_write_at(i, f, buf, count, &position);

	if (result > 0)
		f->f_pos = position;
	return result;
}

static int fb_ioctl(struct inode *i, struct file *f, unsigned int cmd,
			    unsigned long arg)
{
	const struct fb_info *info = fb_get_info();
	if (cmd == FBIOGET_VSCREENINFO) {
		struct fb_var_screeninfo var = { 0 };
		var.xres = var.xres_virtual = info->width;
		var.yres = var.yres_virtual = info->height;
		var.bits_per_pixel = info->bpp;
		var.red = info->red;
		var.green = info->green;
		var.blue = info->blue;
		if (verify_area(VERIFY_WRITE, (void *)arg, sizeof(var)))
			return -EFAULT;
		memcpy_tofs((void *)arg, &var, sizeof(var));
		return 0;
	}
	if (cmd == FBIOGET_FSCREENINFO) {
		struct fb_fix_screeninfo fix = { 0 };
		memcpy(fix.id, "mariofb", 8);
		fix.smem_start = info->phys_addr;
		fix.smem_len = info->size;
		fix.type = 0;
		fix.visual = 2;
		fix.line_length = info->pitch;
		if (verify_area(VERIFY_WRITE, (void *)arg, sizeof(fix)))
			return -EFAULT;
		memcpy_tofs((void *)arg, &fix, sizeof(fix));
		return 0;
	}
	if (cmd == FBIOGET_CONSOLE_STATE) {
		unsigned long flags;
		uint32_t state;

		if (verify_area(VERIFY_WRITE, (void *)arg, sizeof(state)))
			return -EFAULT;
		spin_lock_irqsave(&fb_lock, flags);
		state = fb_console_state;
		f->private_data = (void *)fb_console_generation;
		spin_unlock_irqrestore(&fb_lock, flags);
		memcpy_tofs((void *)arg, &state, sizeof(state));
		return 0;
	}
	return -EINVAL;
}

static int fb_poll(struct file *f, struct poll_context *ctx)
{
	unsigned long flags;
	int mask = 0;

	poll_wait(f, &fb_console_wait, ctx);
	spin_lock_irqsave(&fb_lock, flags);
	if ((unsigned long)f->private_data != fb_console_generation)
		mask = POLLPRI;
	spin_unlock_irqrestore(&fb_lock, flags);
	return mask;
}

void fbdev_console_transition_started(void)
{
	unsigned long flags;

	spin_lock_irqsave(&fb_lock, flags);
	fb_console_transitioning = 1;
	spin_unlock_irqrestore(&fb_lock, flags);
}

void fbdev_console_state_changed(int graphics)
{
	unsigned long flags;

	spin_lock_irqsave(&fb_lock, flags);
	fb_console_transitioning = 0;
	if (fb_console_state != (unsigned int)graphics) {
		fb_console_state = graphics;
		fb_console_generation++;
		spin_unlock_irqrestore(&fb_lock, flags);
		wake_up_all(&fb_console_wait);
		return;
	}
	spin_unlock_irqrestore(&fb_lock, flags);
}

static struct file_operations fb_fops = {
	.open = fb_open,
	.release = fb_release,
	.lseek = fb_lseek,
	.read = fb_read,
	/* Positional syscalls use a private offset and must not update f_pos. */
	.read_at = fb_read_at,
	.write = fb_write,
	.write_at = fb_write_at,
	.ioctl = fb_ioctl,
	.poll = fb_poll,
};

void fbdev_init(void)
{
	if (fb_is_enabled()) {
	#ifdef CONFIG_FB_WRITE_GENERATION_SELF_TEST
		fb_write_generation_selftest();
	#endif
		init_wait_queue(&fb_console_wait);
		register_chrdev(FB_MAJOR, &fb_fops);
	}
}
