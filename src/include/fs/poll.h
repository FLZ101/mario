#ifndef _POLL_H
#define _POLL_H

/* These are specified by iBCS2 */
#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020

/* The rest seem to be more-or-less nonstandard. Check them! */
#define POLLRDNORM	0x0040
#define POLLRDBAND	0x0080
#ifndef POLLWRNORM
#define POLLWRNORM	0x0100
#endif
#ifndef POLLWRBAND
#define POLLWRBAND	0x0200
#endif
#ifndef POLLMSG
#define POLLMSG		0x0400
#endif
#ifndef POLLREMOVE
#define POLLREMOVE	0x1000
#endif
#ifndef POLLRDHUP
#define POLLRDHUP       0x2000
#endif

struct pollfd {
	int fd;
	short events;
	short revents;
};

#ifdef __KERNEL__

#include <wait.h>

struct poll_entry {
	struct file *f;
	wait_queue_t *q;
};

#define MAX_POLL_ENTRIES 32

struct poll_context {
	void (*qproc)(struct file *, wait_queue_t *, struct poll_context *);
	struct poll_entry entries[MAX_POLL_ENTRIES];
	int n;
};

// Basically register the @q to sleep on later
static inline void poll_wait(struct file *f, wait_queue_t *q, struct poll_context *ctx)
{
	if (q && ctx->qproc)
		ctx->qproc(f, q, ctx);
}

#endif

#endif
