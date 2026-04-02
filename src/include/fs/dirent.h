#ifndef _FS_DIRENT_H
#define _FS_DIRENT_H

#include <types.h>

struct mario_dirent {
	ino_t d_ino;
	off_t d_off; // f_pos. End of this entry, and start of next one
	unsigned short d_reclen; // length of this entry
	unsigned char d_type;
	char d_name[256];
};

struct mario_dirent64 {
	ino64_t d_ino;
	off64_t d_off; // f_pos. End of this entry, and start of next one
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

#endif
