#ifndef _ERRNO_H
#define _ERRNO_H

#define ENOMEM		1
#define EINVAL		2
#define EBUSY		3
#define EIO		4
#define ENOENT		5
#define	EACCES		6
#define ENOTDIR		7
#define	ENFILE		8
#define	EMFILE		9	/* Too many open files */
#define ENAMETOOLONG	10
#define EEXIST		11
#define EISDIR		12
#define ENOSPC		13
#define EBADF		14
#define EFAULT		15
#define EPERM		16
#define ENOTEMPTY	17
#define	EXDEV		18
#define ENODEV		19
#define ENOTBLK		20
#define ENXIO		21
#define ECHILD		22
#define E2BIG		23

extern int errno;

#endif	/* _ERRNO_H */