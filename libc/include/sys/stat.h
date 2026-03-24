#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <syscall.h>
#include <sys/types.h>

#include <time.h>

struct stat {
	dev_t st_dev;
	int __st_dev_padding;
	long __st_ino_truncated;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid; // UNUSED
	gid_t st_gid; // UNUSED
	dev_t st_rdev;
	int __st_rdev_padding;
	off_t st_size;
	blksize_t st_blksize;
	blkcnt_t st_blocks;
	struct {
		long tv_sec;
		long tv_nsec;
	} __st_atim32, __st_mtim32, __st_ctim32; // UNUSED
	ino_t st_ino;
	struct timespec st_atim; // UNUSED
	struct timespec st_mtim; // UNUSED
	struct timespec st_ctim; // UNUSED
};

#define S_IFMT  0170000

#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFREG 0100000
#define S_IFIFO 0010000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000

#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode)  (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode)  (((mode) & S_IFMT) == S_IFBLK)
#define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISLNK(mode)  (((mode) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

#define MODE_REG	S_IFREG
#define MODE_DIR	S_IFDIR
#define MODE_BLK	S_IFBLK
#define MODE_CHR	S_IFCHR
#define MODE_FIFO	S_IFIFO

static inline _syscall1(int,mkdir,const char *,pathname)
static inline _syscall3(int,mknod,const char *,pathname, mode_t, mode, dev_t, dev)
static inline _syscall2(int,stat,const char *, pathname, struct stat *,statbuf)
static inline _syscall2(int,fstat,int, fd, struct stat *,statbuf)

#endif
