#ifndef _RESOURCE_H
#define _RESOURCE_H

struct rlimit {
	long	rlim_cur;
	long	rlim_max;
};

#define RLIMIT_DATA	0	/* maximum data size */
#define RLIMIT_STACK	1	/* maximum stack size */

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#define RLIM_INFINITY	(~0UL)

#define INIT_RLIMITS \
{ \
	{RLIM_INFINITY, RLIM_INFINITY},	\
	{8192 * 1024, 8192 * 1024}	\
}

#endif	/* _RESOURCE_H */
