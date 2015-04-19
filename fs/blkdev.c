#include <misc.h>
#include <errno.h>

#include <fs/blkdev.h>

#include <lib/stddef.h>

#define MAX_BLKDEV	64

struct {
	struct blkdev_operations *blkdev_ops;
} blkdevs[MAX_BLKDEV];

extern void rd_init(void);

void __tinit blkdev_init(void)
{
	int i;

	for (i = 0; i < MAX_BLKDEV; i++)
		blkdevs[i].blkdev_ops = NULL;

	rd_init();
}

int register_blkdev(unsigned int major, struct blkdev_operations *blkdev_ops)
{
	if (major == NODEV || major >= MAX_BLKDEV)
		return -EINVAL;

	if (blkdevs[major].blkdev_ops)
		return -EBUSY;

	blkdevs[major].blkdev_ops = blkdev_ops;
	return 0;
}

int unregister_blkdev(unsigned int major)
{
	if (!check_blkdev(major))
		return -EINVAL;

	blkdevs[major].blkdev_ops = NULL;
	return 0;
}

/* check whether @major is registered */
int check_blkdev(unsigned int major)
{
	if (major == NODEV || major >= MAX_BLKDEV)
		return 0;
	if (!blkdevs[major].blkdev_ops)
		return 0;
	return 1;
}

int blkdev_read(dev_t dev, unsigned long sector, unsigned long nr, char *buf)
{
	unsigned int major;
	struct blkdev_operations *blkdev_ops;

	major = MAJOR(dev);
	if (!check_blkdev(major))
		return -EINVAL;

	blkdev_ops = blkdevs[major].blkdev_ops;
	if (blkdev_ops->blkdev_read)
		return blkdev_ops->blkdev_read(dev, sector, nr, buf);
	return 0;
}

int blkdev_write(dev_t dev, unsigned long sector, unsigned long nr, char *buf)
{
	unsigned int major;
	struct blkdev_operations *blkdev_ops;

	major = MAJOR(dev);
	if (!check_blkdev(major))
		return -EINVAL;

	blkdev_ops = blkdevs[major].blkdev_ops;
	if (blkdev_ops->blkdev_write)
		return blkdev_ops->blkdev_write(dev, sector, nr, buf);
	return 0;
}

int blkdev_get_info(dev_t dev, struct blkdev_info *info)
{
	unsigned int major;
	struct blkdev_operations *blkdev_ops;

	major = MAJOR(dev);
	if (!check_blkdev(major))
		return -EINVAL;

	blkdev_ops = blkdevs[major].blkdev_ops;
	if (blkdev_ops->blkdev_get_info)
		return blkdev_ops->blkdev_get_info(dev, info);
	return 0;
}

int get_sector_size(dev_t dev, int *size)
{
	struct blkdev_info info;

	if (blkdev_get_info(dev, &info)) {
		*size = 0;
		return -EINVAL;
	}
	*size = info.sector_size;
	return 0;
}