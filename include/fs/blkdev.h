#ifndef _BLKDEV_H
#define _BLKDEV_H

struct blkdev_info {
	int sector_size;
	int nr;	/* the number of sectors */
};

struct blkdev_operations {
	/* read @nr sector(s) starting at @sector in @dev to @buf */
	int (*blkdev_read)(dev_t dev, unsigned long sector, 
		unsigned long nr, char *buf);
	/* write @buf to @nr sector(s) starting at @sector in @dev */
	int (*blkdev_write)(dev_t dev, unsigned long sector, 
		unsigned long nr, char *buf);
	/* get generic information about a block device */
	int (*blkdev_get_info)(dev_t dev, struct blkdev_info *info);
};

void blkdev_init(void);

int register_blkdev(unsigned int major, struct blkdev_operations *blkdev_ops);
int unregister_blkdev(unsigned int major);

int check_blkdev(unsigned int major);

int blkdev_read(dev_t dev, unsigned long sector, unsigned long nr, char *buf);
int blkdev_write(dev_t dev, unsigned long sector, unsigned long nr, char *buf);

int blkdev_get_info(dev_t dev, struct blkdev_info *info);

int get_sector_size(dev_t dev, int *size);

#define RD_MAJOR	1

#endif /* _BLKDEV_H */