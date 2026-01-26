#ifndef _STAT_H
#define _STAT_H

/*
 * Would that all fields is long type cause trouble?
 */
struct stat {
	unsigned long st_dev;
	unsigned long st_nlink;
	unsigned long st_ino;
	unsigned long st_mode;
	unsigned long st_rdev;	/* ??? */
	unsigned long st_size;
	unsigned long st_blksize;
	unsigned long st_blocks;
};

#define MODE_REG	0
#define MODE_DIR	1
#define MODE_BLK	2
#define MODE_CHR	3
#define MODE_FIFO	4

#define S_ISREG(m)	((m) == MODE_REG)
#define S_ISDIR(m)	((m) == MODE_DIR)
#define S_ISBLK(m)	((m) == MODE_BLK)
#define S_ISCHR(m)	((m) == MODE_CHR)
#define S_ISFIFO(m)	((m) == MODE_FIFO)

#endif /* _STAT_H */
