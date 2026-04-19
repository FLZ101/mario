#ifndef _STAT_H
#define _STAT_H

#include <types.h>
#include <time.h>

#define STATX_TYPE 1U
#define STATX_MODE 2U
#define STATX_NLINK 4U
#define STATX_UID 8U
#define STATX_GID 0x10U
#define STATX_ATIME 0x20U
#define STATX_MTIME 0x40U
#define STATX_CTIME 0x80U
#define STATX_INO 0x100U
#define STATX_SIZE 0x200U
#define STATX_BLOCKS 0x400U
#define STATX_BASIC_STATS 0x7ffU
#define STATX_BTIME 0x800U
#define STATX_ALL 0xfffU

struct statx_timestamp {
	int64_t tv_sec;
	uint32_t tv_nsec, __pad;
};

struct statx {
	uint32_t stx_mask;
	uint32_t stx_blksize;
	uint64_t stx_attributes;
	uint32_t stx_nlink;
	uint32_t stx_uid;
	uint32_t stx_gid;
	uint16_t stx_mode;
	uint16_t __pad0[1];
	uint64_t stx_ino;
	uint64_t stx_size;
	uint64_t stx_blocks;
	uint64_t stx_attributes_mask;
	struct statx_timestamp stx_atime;
	struct statx_timestamp stx_btime;
	struct statx_timestamp stx_ctime;
	struct statx_timestamp stx_mtime;
	uint32_t stx_rdev_major;
	uint32_t stx_rdev_minor;
	uint32_t stx_dev_major;
	uint32_t stx_dev_minor;
	uint64_t __pad1[14];
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
#define MODE_LNK	S_IFLNK

#ifndef S_IRUSR
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG 0070
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXO 0007
#endif

#endif /* _STAT_H */
