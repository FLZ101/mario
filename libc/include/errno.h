#ifndef _ERRNO_H
#define _ERRNO_H

#define USE(name, code, msg) name = code,

enum {
	#include "errno.def"
	_MAX_ERRNO
};

#undef USE

extern int errno;

#endif	/* _ERRNO_H */
