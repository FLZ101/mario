#ifndef _RESOURCE_H
#define _RESOURCE_H

typedef unsigned long long rlim_t;

struct rlimit {
	rlim_t	rlim_cur;
	rlim_t	rlim_max;
};

#define RLIMIT_DATA		0	/* maximum data size */
#define RLIMIT_STACK	1	/* maximum stack size */
#define RLIMIT_NLIMITS	2

#define RLIM_NLIMITS RLIMIT_NLIMITS

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#define RLIM_INFINITY	(~0UL)

#define INIT_RLIMITS \
{ \
	{RLIM_INFINITY, RLIM_INFINITY},	\
	{8192 * 1024, 8192 * 1024 * 16}	\
}

struct rusage {
	struct timeval64 ru_utime; /* user CPU time used */
	struct timeval64 ru_stime; /* system CPU time used */
	long ru_maxrss;          /* maximum resident set size */
	long ru_ixrss;           /* unused */
	long ru_idrss;           /* unused */
	long ru_isrss;           /* unused */
	long ru_minflt;          /* page reclaims (soft page faults) */
	long ru_majflt;          /* page faults (hard page faults) */
	long ru_nswap;           /* unused */
	long ru_inblock;         /* block input operations */
	long ru_oublock;         /* block output operations */
	long ru_msgsnd;          /* unused */
	long ru_msgrcv;          /* unused */
	long ru_nsignals;        /* unused */
	long ru_nvcsw;           /* voluntary context switches */
	long ru_nivcsw;          /* involuntary context switches */
};

#endif	/* _RESOURCE_H */
