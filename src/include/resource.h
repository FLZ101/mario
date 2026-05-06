#ifndef _RESOURCE_H
#define _RESOURCE_H

typedef unsigned long long rlim_t;

struct rlimit {
	rlim_t	rlim_cur;
	rlim_t	rlim_max;
};

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9
#define RLIMIT_NLIMITS	10

#define RLIM_NLIMITS RLIMIT_NLIMITS

/*
 * SuS says limits have to be unsigned.
 * Which makes a ton more sense anyway.
 */
#define RLIM_INFINITY	(~0UL)

#define INIT_RLIMITS \
{ \
	[RLIMIT_DATA] = {RLIM_INFINITY, RLIM_INFINITY},	\
	[RLIMIT_STACK] = {8192 * 1024, 8192 * 1024 * 16},	\
	[RLIMIT_NOFILE] = {NR_OPEN, NR_OPEN}	\
}

struct rusage {
	struct timeval64 ru_utime; /* user CPU time used */
	struct timeval64 ru_stime; /* system CPU time used */
	long ru_maxrss;
	long ru_ixrss;
	long ru_idrss;
	long ru_isrss;
	long ru_minflt;
	long ru_majflt;
	long ru_nswap;
	long ru_inblock;
	long ru_oublock;
	long ru_msgsnd;
	long ru_msgrcv;
	long ru_nsignals;
	long ru_nvcsw;
	long ru_nivcsw;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

#endif	/* _RESOURCE_H */
