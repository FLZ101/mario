#include <fs/fs.h>
#include <mm/kmalloc.h>

static void qproc(struct file *f, wait_queue_t *q, struct poll_context *ctx)
{
    assert(ctx->n < MAX_POLL_ENTRIES);
    ctx->entries[ctx->n++] = (struct poll_entry) { .f = f, .q = q };
}

#define FD_IGNORE -1001

int poll_fds(struct poll_context *ctx, struct pollfd *fds, unsigned int nfds)
{
    int n_done = 0;
    for (unsigned int i = 0; i < nfds; ++i) {
        unsigned int fd = fds[i].fd;
        short events = fds[i].events;
        short *revents = &fds[i].revents;

        if (FD_IGNORE == fd)
            continue;

        struct file *f = current->files->fd[fd];
        short mask = f->f_op->poll(f, ctx);
        *revents = (events | POLLERR | POLLHUP) & mask;
        if (*revents)
            ++n_done;
    }
    return n_done;
}

int do_poll(struct pollfd *fds, unsigned int nfds, long timeout)
{
    int n_done = 0;
    for (unsigned int i = 0; i < nfds; ++i) {
        unsigned int fd = fds[i].fd;
        short events = fds[i].events;
        short *revents = &fds[i].revents;

        if (FD_IGNORE == fd)
            continue;

        struct file *f;
        if (fd >= NR_OPEN || !(f = current->files->fd[fd])) {
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
    }
    if (n_done > 0 || !timeout)
        return n_done;

    struct poll_context ctx = { .qproc = qproc };

    n_done = poll_fds(&ctx, fds, nfds);
    if (n_done > 0)
        return n_done;

    wait_queue_node_t node;
    init_wait_queue_node(&node, current);

    int n_wait = 0;
    for (unsigned int i = 0; i < ctx.n; ++i) {
        if (ctx.entries[i].q) {
            in_wait_queue(ctx.entries[i].q, &node);
            ++n_wait;
        }
    }
    if (!n_wait)
        return 0;

    if (timeout < 0) {
        current->state = TASK_INTERRUPTIBLE;
        schedule();
    } else {
        schedule_timeout((timeout * HZ + 999) / 1000);
    }

    for (unsigned int i = 0; i < ctx.n; ++i) {
        if (ctx.entries[i].q)
            out_wait_queue(ctx.entries[i].q, &node);
    }

    ctx.qproc = NULL;
    n_done = poll_fds(&ctx, fds, nfds);
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
