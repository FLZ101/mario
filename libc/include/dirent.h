#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

struct dirent {
	ino_t d_ino;
	off_t d_off; // f_pos. End of this entry, and start of next one
	unsigned short d_reclen; // length of this entry
	unsigned char d_type;
	char d_name[256];
};

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12
#define DT_WHT 14
#define IFTODT(x) ((x)>>12 & 017)
#define DTTOIF(x) ((x)<<12)

ssize_t getdents(int fd, void *dirp, size_t count);

struct __dirstream {
	int fd;                   // file descriptor from openat(..., O_DIRECTORY)
	char *data;               // buffer holding raw directory entries (struct dirent)
	size_t allocation;        // size of 'data' buffer
	size_t size;              // number of valid bytes in 'data'
	size_t offset;            // current read offset in 'data'
	off_t filepos;            // current file position (for telldir/seekdir)
};

typedef struct __dirstream DIR;

DIR *opendir(const char *name);

#endif
