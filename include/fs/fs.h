#ifndef _FS_H
#define _FS_H

struct file_operations {
	int (*lseek)(void);
};

#endif /* _FS_H */