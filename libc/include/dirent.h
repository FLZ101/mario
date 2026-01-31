#ifndef _DIRENT_H
#define _DIRENT_H

#include <sys/types.h>

struct mario_dirent {
	unsigned long d_ino;
	unsigned long d_off; // f_pos. End of this entry, and start of next one
	unsigned short d_reclen; // length of this entry
	char d_name[1];
};

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
