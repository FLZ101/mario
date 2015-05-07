#ifndef _STAT_H
#define _STAT_H

struct stat {
	unsigned short st_dev;
	unsigned short st_ino;
	unsigned short st_mode;
	unsigned short st_nlink;
	unsigned short st_rdev;
	unsigned long  st_size;
};

#define MODE_REG	0
#define MODE_DIR	1
#define MODE_BLK	2
#define MODE_CHR	3

#define S_ISREG(m)	((m) == MODE_REG)
#define S_ISDIR(m)	((m) == MODE_DIR)
#define S_ISBLK(m)	((m) == MODE_BLK)
#define S_ISCHR(m)	((m) == MODE_CHR)

#endif /* _STAT_H */