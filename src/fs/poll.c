#include <fs/fs.h>
#include <mm/kmalloc.h>
#include <signal.h>

#define PPOLL_SIGSET_SIZE 8

static void count_qproc(struct file *f, wait_queue_t *q, struct poll_context *ctx)
{
    (void) f;
    (void) q;

    if (ctx->n == (unsigned int)-1) {
        ctx->error = -ENOMEM;
        return;
    }
    ctx->n++;
}

static void collect_qproc(struct file *f, wait_queue_t *q,
                          struct poll_context *ctx)
{
    struct poll_entry *entry;

    if (ctx->n == ctx->max) {
        ctx->error = -ENOMEM;
        return;
    }

    entry = &ctx->entries[ctx->n++];
    entry->f = f;
    entry->q = q;
    init_wait_queue_node(&entry->node, current);
}

#define FD_IGNORE -1001

static void poll_get_files(struct file **files, struct pollfd *fds,
                           unsigned int nfds)
{
    unsigned int i;

    for (i = 0; i < nfds; i++) {
        int fd = fds[i].fd;
        struct file *f;

        files[i] = NULL;
        if (fd == FD_IGNORE || fd < 0 || fd >= NR_OPEN)
            continue;
        f = current->files->fd[fd];
        if (!f)
            continue;
        fref(f);
        files[i] = f;
    }
}

static void poll_put_files(struct file **files, unsigned int nfds)
{
    unsigned int i;

    for (i = 0; i < nfds; i++)
        if (files[i])
            release_file(files[i], 1);
}

static int poll_fds(struct poll_context *ctx, struct pollfd *fds,
                    struct file **files, unsigned int nfds)
{
    int n_done = 0;
    for (unsigned int i = 0; i < nfds; ++i) {
        int fd = fds[i].fd;
        short events = fds[i].events;
        short *revents = &fds[i].revents;

        if (FD_IGNORE == fd)
            continue;

        struct file *f = files[i];
        if (fd < 0 || fd >= NR_OPEN || !f) {
            *revents = POLLNVAL;
            ++n_done;
            continue;
        }
        if (!f->f_op->poll) {
            *revents = events & (POLLIN | POLLOUT);
            ++n_done;
            continue;
        }
        *revents = 0;

        short mask = f->f_op->poll(f, ctx);
        *revents = (events | POLLERR | POLLHUP) & mask;
        if (*revents)
            ++n_done;
    }
    return n_done;
}

int do_poll(struct pollfd *fds, unsigned int nfds, long timeout)
{
    struct poll_context ctx = { .qproc = count_qproc };
    struct file **files;
    struct poll_entry *entries = NULL;
    int n_done;

    if (nfds > (unsigned int)-1 / sizeof(*files))
        return -ENOMEM;

    files = kmalloc(nfds * sizeof(*files));
    if (!files)
        return -ENOMEM;
    poll_get_files(files, fds, nfds);

    n_done = poll_fds(&ctx, fds, files, nfds);
    if (ctx.error) {
        n_done = ctx.error;
        goto out;
    }
    if (n_done > 0 || !timeout)
        goto out;

    if (!ctx.n)
        goto out;
    if (ctx.n > (unsigned int)-1 / sizeof(*entries)) {
        n_done = -ENOMEM;
        goto out;
    }
    entries = kmalloc(ctx.n * sizeof(*entries));
    if (!entries) {
        n_done = -ENOMEM;
        goto out;
    }

    ctx.entries = entries;
    ctx.max = ctx.n;
    ctx.n = 0;
    ctx.error = 0;
    ctx.qproc = collect_qproc;
    n_done = poll_fds(&ctx, fds, files, nfds);
    if (ctx.error) {
        n_done = ctx.error;
        goto out;
    }
    if (n_done > 0 || !ctx.n)
        goto out;

    current->state = TASK_INTERRUPTIBLE;
    for (unsigned int i = 0; i < ctx.n; ++i) {
        in_wait_queue(ctx.entries[i].q, &ctx.entries[i].node);
    }

    ctx.qproc = NULL;
    n_done = poll_fds(&ctx, fds, files, nfds);
    if (n_done > 0 || current->state != TASK_INTERRUPTIBLE)
        goto detach;

    if (timeout < 0) {
        schedule();
    } else {
        schedule_timeout((timeout * HZ + 999) / 1000);
    }

    n_done = poll_fds(&ctx, fds, files, nfds);

detach:
    current->state = TASK_RUNNING;
    for (unsigned int i = 0; i < ctx.n; ++i) {
        out_wait_queue(ctx.entries[i].q, &ctx.entries[i].node);
    }

out:
    if (entries)
        kfree(entries);
    poll_put_files(files, nfds);
    kfree(files);
    return n_done;
}

/*
 * The timeout argument specifies the number of milliseconds that poll()
 * should block waiting for a file descriptor to become ready.
 */
int sys_poll(struct pollfd *ufds, unsigned int nfds, long timeout)
{
    int error = 0;

    if (!nfds)
        return 0;

    unsigned int sz = nfds * sizeof(struct pollfd);

    error = verify_area(VERIFY_READ, ufds, sz);
    if (error)
        return error;
    error = verify_area(VERIFY_WRITE, ufds, sz);
    if (error)
        return error;

    struct pollfd *fds = (struct pollfd *) kmalloc(sz);
    if (!fds)
        return -ENOMEM;
    memcpy_fromfs(fds, ufds, sz);

    error = do_poll(fds, nfds, timeout);
    memcpy_tofs(ufds, fds, sz);

    kfree(fds);
    return error;
}

int sys_ppoll(struct pollfd *ufds, unsigned int nfds,
	      const struct timespec *timeout, const sigset_t *sigmask,
	      size_t sigsetsize)
{
	struct timespec ts;
	sigset_t old_mask;
	const long max_timeout = (long)(~0UL >> 1);
	long timeout_ms = -1;
	int error;

	if (timeout) {
		error = verify_area(VERIFY_READ, timeout, sizeof(ts));
		if (error)
			return error;
		memcpy_fromfs(&ts, timeout, sizeof(ts));
		if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
			return -EINVAL;
		if (ts.tv_sec > (max_timeout - 999) / 1000)
			timeout_ms = max_timeout;
		else
			timeout_ms = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
	}

	old_mask = current->blocked;
	if (sigmask) {
		sigset_t mask;

		if (sigsetsize != PPOLL_SIGSET_SIZE)
			return -EINVAL;
		error = verify_area(VERIFY_READ, sigmask, sigsetsize);
		if (error)
			return error;
		memcpy_fromfs(&mask, sigmask, sizeof(mask));
		current->blocked = mask & ~( (1UL << (SIGKILL - 1)) |
					    (1UL << (SIGSTOP - 1)) );
	}

	error = sys_poll(ufds, nfds, timeout_ms);
	if (!error && (current->signal & ~current->blocked))
		error = -EINTR;
	current->blocked = old_mask;
	return error;
}

/*
 * nfds: the highest-numbered file descriptor in any of the three sets, plus 1.
 */
int sys_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout)
{
    int error = 0;

    if (nfds < 0)
        return -EINVAL;
    if (!nfds)
        return 0;

    long timeout_ms = -1;
    if (timeout) {
        error = verify_area(VERIFY_READ, timeout, sizeof(*timeout));
        if (error)
            return error;
        struct timeval tmp;
        memcpy_fromfs(&tmp, timeout, sizeof(*timeout));
        timeout_ms = 1000 * tmp.tv_sec + (tmp.tv_usec + 999) / 1000;
    }

    if (readfds) {
        error = verify_area(VERIFY_READ, readfds, sizeof(fd_set));
        if (error)
            return error;
        error = verify_area(VERIFY_WRITE, readfds, sizeof(fd_set));
        if (error)
            return error;
    }
    if (writefds) {
        error = verify_area(VERIFY_READ, writefds, sizeof(fd_set));
        if (error)
            return error;
        error = verify_area(VERIFY_WRITE, writefds, sizeof(fd_set));
        if (error)
            return error;
    }
    if (exceptfds) {
        error = verify_area(VERIFY_READ, exceptfds, sizeof(fd_set));
        if (error)
            return error;
        error = verify_area(VERIFY_WRITE, exceptfds, sizeof(fd_set));
        if (error)
            return error;
    }

    fd_set *rfds = NULL, *wfds = NULL, *efds = NULL;
    if (readfds) {
        rfds = kmalloc(sizeof(fd_set));
        if (!rfds) {
            error = -ENOMEM;
            goto tail_1;
        }
        memcpy_fromfs(rfds, readfds, sizeof(fd_set));
    }
    if (writefds) {
        wfds = kmalloc(sizeof(fd_set));
        if (!wfds) {
            error = -ENOMEM;
            goto tail_1;
        }
        memcpy_fromfs(wfds, writefds, sizeof(fd_set));
    }
    if (exceptfds) {
        efds = kmalloc(sizeof(fd_set));
        if (!efds) {
            error = -ENOMEM;
            goto tail_1;
        }
        memcpy_fromfs(efds, exceptfds, sizeof(fd_set));
    }

    struct pollfd *poll_fds = kmalloc(nfds * sizeof(struct pollfd));
    if (!poll_fds) {
        error = -ENOMEM;
        goto tail_1;
    }

    for (int i = 0; i < nfds; ++i) {
        poll_fds[i].fd = FD_IGNORE;
        poll_fds[i].events = 0;
        poll_fds[i].revents = 0;

        if (rfds && FD_ISSET(i, rfds)) {
            poll_fds[i].fd = i;
            poll_fds[i].events |= POLLIN;
        }
        if (wfds && FD_ISSET(i, wfds)) {
            poll_fds[i].fd = i;
            poll_fds[i].events |= POLLOUT;
        }
        if (efds && FD_ISSET(i, efds)) {
            poll_fds[i].fd = i;
            poll_fds[i].events |= POLLERR;
        }
    }

    error = do_poll(poll_fds, nfds, timeout_ms);
    if (error)
        goto tail_2;

    if (rfds)
        memset(rfds, 0, sizeof(fd_set));
    if (wfds)
        memset(wfds, 0, sizeof(fd_set));
    if (efds)
        memset(efds, 0, sizeof(fd_set));

    int n_done = 0;
    for (int i = 0; i < nfds; i++) {
        if (poll_fds[i].fd == FD_IGNORE)
            continue;
        if (poll_fds[i].revents == 0)
            continue;

        // Map POLLIN
        if (poll_fds[i].revents & POLLIN) {
            if (rfds) {
                FD_SET(i, rfds);
                ++n_done;
            }
        }

        // Map POLLOUT
        if (poll_fds[i].revents & POLLOUT) {
            if (wfds) {
                FD_SET(i, wfds);
                ++n_done;
            }
        }

        // Map Errors/Exceptions
        if (poll_fds[i].revents & (POLLERR | POLLHUP | POLLPRI)) {
            if (efds) {
                FD_SET(i, efds);
                ++n_done;
            }
        }
    }
    error = n_done;

    if (rfds)
        memcpy_tofs(readfds, rfds, sizeof(fd_set));
    if (wfds)
        memcpy_tofs(writefds, wfds, sizeof(fd_set));
    if (efds)
        memcpy_tofs(exceptfds, efds, sizeof(fd_set));

tail_2:
    kfree(poll_fds);
tail_1:
    if (rfds)
        kfree(rfds);
    if (wfds)
        kfree(wfds);
    if (efds)
        kfree(efds);
    return error;
}
