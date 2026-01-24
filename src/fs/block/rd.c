#include <lib/string.h>

#include <multiboot.h>
#include <errno.h>
#include <misc.h>

#include <fs/fs.h>
#include <fs/blkdev.h>

#define RD_SECTOR_SIZE 512

#define MAX_RD 3

struct {
	unsigned long rd_start;
	unsigned long rd_end;
	unsigned long rd_nr;	/* the number of sectors */
	char *name;
} rd_info[MAX_RD];

static int nr_rd;	/* the number of ramdisks loaded */

extern unsigned long end;

void __tinit ramdisk_setup(struct multiboot_info *m)
{
	int i;

	end = 0;

	if (!(MB_FLAG_MODULE & m->flags))
		goto tail;

	struct multiboot_module *mod =
		(struct multiboot_module *)m->mods_addr;

	for (nr_rd = 0, i = 0; nr_rd < MAX_RD && i < m->mods_count; i++) {
		/* all boot modules loaded are page-aligned */
		unsigned long __start = mod[i].mod_start;
		unsigned long __end = PAGE_ALIGN(mod[i].mod_end);

		rd_info[nr_rd].rd_start = __start;
		rd_info[nr_rd].rd_end = __end;
		rd_info[nr_rd].rd_nr = (__end - __start) / RD_SECTOR_SIZE;
		rd_info[nr_rd].name = (char *)mod[i].string;

		if (end < __end)
			end = __end;
		nr_rd++;
	}
tail:
	if (!end)
		early_hang("no ramdisk loaded!\n");
	end += KERNEL_BASE;	/* !!! */

	early_print("ramdisk(s):\n");
	for (i = 0; i < nr_rd; i++) {
		early_print("rd_start=%x, rd_end=%x, name=%s\n",
			rd_info[i].rd_start += KERNEL_BASE,
			rd_info[i].rd_end += KERNEL_BASE,
			rd_info[i].name
		);
	}
}

int rd_read(dev_t dev, unsigned long sector, unsigned long nr, char *buf)
{
	unsigned int minor;
	unsigned long rd_nr;

	minor = MINOR(dev);
	if (minor >= nr_rd)
		return -EINVAL;

	rd_nr = rd_info[minor].rd_nr;
	if (sector >= rd_nr)
		return -EINVAL;
	if (sector + nr >= rd_nr)
		nr = rd_nr - sector;

	if (nr)
		memcpy(buf, (void *)(rd_info[minor].rd_start +
			sector*RD_SECTOR_SIZE), nr*RD_SECTOR_SIZE);
	return 0;
}

int rd_write(dev_t dev, unsigned long sector, unsigned long nr, char *buf)
{
	unsigned int minor;
	unsigned long rd_nr;

	minor = MINOR(dev);
	if (minor >= nr_rd)
		return -EINVAL;

	rd_nr = rd_info[minor].rd_nr;
	if (sector >= rd_nr)
		return -EINVAL;
	if (sector + nr >= rd_nr)
		nr = rd_nr - sector;

	if (nr)
		memcpy((void *)(rd_info[minor].rd_start +
			sector*RD_SECTOR_SIZE), buf, nr*RD_SECTOR_SIZE);
	return 0;
}

int rd_get_info(dev_t dev, struct blkdev_info *info)
{
	unsigned int minor;

	minor = MINOR(dev);
	if (minor >= nr_rd)
		return -EINVAL;

	info->sector_size = RD_SECTOR_SIZE;
	info->nr = rd_info[minor].rd_nr;
	return 0;
}

struct blkdev_operations rd_ops = {
	rd_read,
	rd_write,
	rd_get_info
};

void __tinit rd_init(void)
{
	register_blkdev(RD_MAJOR, &rd_ops);
}