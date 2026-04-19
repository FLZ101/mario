#include <lib/string.h>

#include <multiboot.h>
#include <errno.h>
#include <misc.h>

#include <fs/fs.h>
#include <fs/blkdev.h>
#include <fs/mariofs/mariofs.h>

#define RD_SECTOR_SIZE 512

#define MAX_RD 6

struct {
	unsigned long rd_start;
	unsigned long rd_end;
	unsigned long rd_nr;	/* the number of sectors */
	char *name;
} rd_info[MAX_RD];

static int nr_rd = 0;	/* the number of ramdisks loaded */

unsigned long last_rd_end = 0;

void ramdisk_setup(struct multiboot_info *m)
{
	int i;

	if (!(MB_FLAG_MODULE & m->flags))
		goto tail;

	struct multiboot_module *mod =
		(struct multiboot_module *)m->mods_addr;

	for (nr_rd = 0, i = 0; i < m->mods_count; i++) {
		unsigned long start = mod[i].mod_start + KERNEL_BASE;
		unsigned long end = mod[i].mod_end + KERNEL_BASE;

		// Ramdisks are formatted with MarioFS. Multiple ramdisks may be concatenated
		// (e.g., via cat) and loaded as a single module.
		unsigned long j = 0;
		while (start + j < end) {
			if (nr_rd >= MAX_RD)
				hang("Too many ramdisks!\n");

			assert(PAGE_ALIGNED(start + j));
			assert(start + j + RD_SECTOR_SIZE < end);

			struct mario_super_block *sb = (struct mario_super_block *) (start + j);
			assert(sb->magic == MARIO_MAGIC);
			assert(sb->sector_size == RD_SECTOR_SIZE);
			unsigned long rd_size = sb->nr_blocks * sb->sector_size;
			assert(PAGE_ALIGNED(rd_size));

			rd_info[nr_rd].rd_start = start + j;
			rd_info[nr_rd].rd_end = start + j + rd_size;
			rd_info[nr_rd].rd_nr = sb->nr_blocks;
			rd_info[nr_rd].name = (char *)mod[i].string;

			++nr_rd;

			j += rd_size;
		}
		assert(start + j == end);
	}
tail:
	if (!nr_rd)
		hang("No ramdisk loaded!\n");

	last_rd_end = rd_info[nr_rd - 1].rd_end;

	printk("Ramdisk(s):\n");
	for (i = 0; i < nr_rd; i++) {
		printk("rd_start=%x, rd_end=%x, name=%s\n",
			rd_info[i].rd_start,
			rd_info[i].rd_end,
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
	.blkdev_read = rd_read,
	.blkdev_write = rd_write,
	.blkdev_get_info = rd_get_info
};

void rd_init(void)
{
	register_blkdev(RD_MAJOR, &rd_ops);
}