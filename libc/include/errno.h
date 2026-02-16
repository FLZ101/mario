#ifndef _ERRNO_H
#define _ERRNO_H

#define USE(name, code, msg) name = code,

enum {
	#include "errno.def"
	_MAX_ERRNO
};

#undef USE

extern int errno;

#define __set_errno(e) do { errno = (e); } while (0)

#endif	/* _ERRNO_H */
