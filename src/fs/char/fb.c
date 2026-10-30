#include <fs/fs.h>
#include <video/fb.h>

static int fb_writer_open;
static spinlock_t fb_lock = SPINLOCK_UNLOCKED;

static int fb_open(struct inode *i, struct file *f)
{
	unsigned long flags;
	if (!fb_is_enabled())
		return -ENODEV;
	if (!(f->f_mode & 2))
		return 0;
	spin_lock_irqsave(&fb_lock, flags);
	if (fb_writer_open) {
		spin_unlock_irqrestore(&fb_lock, flags);
		return -EBUSY;
	}
	fb_writer_open = 1;
	spin_unlock_irqrestore(&fb_lock, flags);
	return 0;
}

static void fb_release(struct inode *i, struct file *f)
{
	unsigned long flags;
	if (!(f->f_mode & 2))
		return;
	spin_lock_irqsave(&fb_lock, flags);
	fb_writer_open = 0;
	spin_unlock_irqrestore(&fb_lock, flags);
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
	const struct fb_info *info = fb_get_info();
	if (count < 0)
		return -EINVAL;
	if (f->f_pos >= info->size)
		return 0;
	if (count > info->size - f->f_pos)
		count = info->size - f->f_pos;
	memcpy_tofs(buf, (char *)info->virt_addr + f->f_pos, count);
	f->f_pos += count;
	return count;
}

static int fb_write(struct inode *i, struct file *f, char *buf, int count)
{
	const struct fb_info *info = fb_get_info();
	if (count < 0)
		return -EINVAL;
	if (f->f_pos >= info->size)
		return count ? -ENOSPC : 0;
	if (count > info->size - f->f_pos)
		count = info->size - f->f_pos;
	memcpy_fromfs((char *)info->virt_addr + f->f_pos, buf, count);
	f->f_pos += count;
	return count;
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
	return -EINVAL;
}

static struct file_operations fb_fops = {
	.open = fb_open,
	.release = fb_release,
	.lseek = fb_lseek,
	.read = fb_read,
	.write = fb_write,
	.ioctl = fb_ioctl,
};

void fbdev_init(void)
{
	if (fb_is_enabled())
		register_chrdev(FB_MAJOR, &fb_fops);
}
