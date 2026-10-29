#include <fs/socket.h>
#include <mm/kmalloc.h>
#include <misc.h>

#define SYS_SOCKETPAIR 8
#define SYS_SEND 9
#define SYS_RECV 10
#define SYS_SENDTO 11
#define SYS_RECVFROM 12
#define SYS_SHUTDOWN 13

extern int sys_read(unsigned int fd, char *buf, unsigned int count);
extern int sys_write(unsigned int fd, char *buf, unsigned int count);

struct unix_socket_pair;

struct unix_socket {
	struct unix_socket_pair *pair;
	struct ring_buffer receive;
	wait_queue_t wait_read;
	wait_queue_t wait_write;
	int opens;
	int read_shutdown;
	int write_shutdown;
};

struct unix_socket_pair {
	struct unix_socket endpoint[2];
	spinlock_t lock;
};

static struct unix_socket *socket_endpoint(struct file *f)
{
	return f->private_data;
}

static struct unix_socket *socket_peer(struct unix_socket *socket)
{
	return &socket->pair->endpoint[socket == &socket->pair->endpoint[0]];
}

static int socket_read(struct inode *i, struct file *f, char *buf, int count)
{
	struct unix_socket *socket = socket_endpoint(f);
	struct unix_socket *peer = socket_peer(socket);
	unsigned long flags;
	int n;

	for (;;) {
		spin_lock_irqsave(&socket->pair->lock, flags);
		if (!ring_buffer_empty(&socket->receive)) {
			n = ring_buffer_read(&socket->receive, buf, count, 1);
			wake_up_interruptible(&socket->wait_write);
			spin_unlock_irqrestore(&socket->pair->lock, flags);
			return n;
		}
		if (!peer->opens || peer->write_shutdown) {
			spin_unlock_irqrestore(&socket->pair->lock, flags);
			return 0;
		}
		spin_unlock_irqrestore(&socket->pair->lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		sleep_on(&socket->wait_read, TASK_INTERRUPTIBLE, NULL);
	}
}

static int socket_write(struct inode *i, struct file *f, char *buf, int count)
{
	struct unix_socket *socket = socket_endpoint(f);
	struct unix_socket *peer = socket_peer(socket);
	unsigned long flags;
	int n;

	for (;;) {
		spin_lock_irqsave(&socket->pair->lock, flags);
		if (socket->write_shutdown || !peer->opens || peer->read_shutdown) {
			spin_unlock_irqrestore(&socket->pair->lock, flags);
			send_sig(SIGPIPE, current, 0);
			return -EPIPE;
		}
		n = ring_buffer_write(&peer->receive, buf, count, 1);
		if (n) {
			wake_up_interruptible(&peer->wait_read);
			spin_unlock_irqrestore(&socket->pair->lock, flags);
			return n;
		}
		spin_unlock_irqrestore(&socket->pair->lock, flags);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		sleep_on(&peer->wait_write, TASK_INTERRUPTIBLE, NULL);
	}
}

static int socket_lseek(struct inode *i, struct file *f, off_t offset, int origin)
{
	return -ESPIPE;
}

static int socket_poll(struct file *f, struct poll_context *ctx)
{
	struct unix_socket *socket = socket_endpoint(f);
	struct unix_socket *peer = socket_peer(socket);
	unsigned long flags;
	short mask = 0;

	poll_wait(f, &socket->wait_read, ctx);
	poll_wait(f, &peer->wait_write, ctx);
	spin_lock_irqsave(&socket->pair->lock, flags);
	if (!ring_buffer_empty(&socket->receive))
		mask |= POLLIN;
	if (!socket->write_shutdown && !peer->read_shutdown &&
	    ring_buffer_space(&peer->receive) && peer->opens)
		mask |= POLLOUT;
	if (!peer->opens || peer->write_shutdown)
		mask |= POLLHUP;
	spin_unlock_irqrestore(&socket->pair->lock, flags);
	return mask;
}

static void socket_release(struct inode *i, struct file *f)
{
	struct unix_socket *socket = socket_endpoint(f);
	struct unix_socket *peer = socket_peer(socket);
	struct unix_socket_pair *pair = socket->pair;
	unsigned long flags;
	int free_pair = 0;

	spin_lock_irqsave(&pair->lock, flags);
	assert(socket->opens > 0);
	if (!--socket->opens) {
		wake_up_interruptible(&peer->wait_read);
		wake_up_interruptible(&peer->wait_write);
		if (!peer->opens)
			free_pair = 1;
	}
	spin_unlock_irqrestore(&pair->lock, flags);
	if (free_pair) {
		ring_buffer_destroy(&pair->endpoint[0].receive);
		ring_buffer_destroy(&pair->endpoint[1].receive);
		kfree(pair);
	}
}

static int socket_open(struct inode *i, struct file *f)
{
	struct unix_socket *socket = socket_endpoint(f);
	unsigned long flags;

	spin_lock_irqsave(&socket->pair->lock, flags);
	socket->opens++;
	spin_unlock_irqrestore(&socket->pair->lock, flags);
	return 0;
}

static struct file_operations socket_fops = {
	.open = socket_open,
	.release = socket_release,
	.lseek = socket_lseek,
	.read = socket_read,
	.write = socket_write,
	.poll = socket_poll,
};

static int next_fd(int start)
{
	for (int fd = start; fd < NR_OPEN; fd++)
		if (!current->files->fd[fd])
			return fd;
	return -EMFILE;
}

int do_socketpair(int domain, int type, int protocol, int fd[2])
{
	struct unix_socket_pair *pair;
	struct inode *inode[2];
	struct file *file[2];
	int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (domain != AF_UNIX || base_type != SOCK_STREAM || protocol)
		return -EOPNOTSUPP;
	fd[0] = next_fd(0);
	if (fd[0] < 0)
		return fd[0];
	fd[1] = next_fd(fd[0] + 1);
	if (fd[1] < 0)
		return fd[1];
	if (get_two_empty_files(file))
		return -ENFILE;
	inode[0] = get_socket_inode();
	inode[1] = get_socket_inode();
	if (!inode[0] || !inode[1])
		return -ENFILE;
	pair = kmalloc(sizeof(*pair));
	if (!pair)
		return -ENOMEM;
	memset(pair, 0, sizeof(*pair));
	INIT_LOCK(&pair->lock);
	for (int i = 0; i < 2; i++) {
		if (ring_buffer_init(&pair->endpoint[i].receive))
			return -ENOMEM;
		pair->endpoint[i].pair = pair;
		pair->endpoint[i].opens = 1;
		init_wait_queue(&pair->endpoint[i].wait_read);
		init_wait_queue(&pair->endpoint[i].wait_write);
		file[i]->f_inode = inode[i];
		file[i]->f_op = &socket_fops;
		file[i]->private_data = &pair->endpoint[i];
		file[i]->f_mode = 3;
		file[i]->f_flags = O_RDWR | (type & (SOCK_NONBLOCK | SOCK_CLOEXEC));
		if (type & SOCK_CLOEXEC)
			FD_SET(fd[i], &current->files->close_on_exec);
		current->files->fd[fd[i]] = file[i];
	}
	return 0;
}

int sys_socketpair(int domain, int type, int protocol, int *fd)
{
	int pair[2];
	int err = verify_area(VERIFY_WRITE, fd, sizeof(pair));

	if (err)
		return err;
	err = do_socketpair(domain, type, protocol, pair);
	if (!err)
		memcpy_tofs(fd, pair, sizeof(pair));
	return err;
}

int sys_shutdown(int fd, int how)
{
	struct file *file;
	struct unix_socket *socket;
	struct unix_socket *peer;
	unsigned long flags;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	if (how < SHUT_RD || how > SHUT_RDWR)
		return -EINVAL;
	socket = socket_endpoint(file);
	peer = socket_peer(socket);
	spin_lock_irqsave(&socket->pair->lock, flags);
	if (how != SHUT_WR) {
		socket->read_shutdown = 1;
		ring_buffer_clear(&socket->receive);
		wake_up_interruptible(&socket->wait_read);
		wake_up_interruptible(&socket->wait_write);
	}
	if (how != SHUT_RD) {
		socket->write_shutdown = 1;
		wake_up_interruptible(&peer->wait_read);
	}
	spin_unlock_irqrestore(&socket->pair->lock, flags);
	return 0;
}

int sys_socketcall(int call, unsigned long *args)
{
	if (call != SYS_SOCKETPAIR && call != SYS_SEND && call != SYS_RECV &&
	    call != SYS_SENDTO && call != SYS_RECVFROM && call != SYS_SHUTDOWN)
		return -ENOSYS;
	if (verify_area(VERIFY_READ, args, 4 * sizeof(unsigned long)))
		return -EFAULT;
	switch (call) {
	case SYS_SOCKETPAIR:
		return sys_socketpair(get_fs_long((int *)&args[0]),
				      get_fs_long((int *)&args[1]),
				      get_fs_long((int *)&args[2]),
				      (int *)get_fs_long((int *)&args[3]));
	case SYS_SEND:
	case SYS_SENDTO:
		return sys_write(get_fs_long((int *)&args[0]),
				 (char *)get_fs_long((int *)&args[1]),
				 get_fs_long((int *)&args[2]));
	case SYS_RECV:
	case SYS_RECVFROM:
		return sys_read(get_fs_long((int *)&args[0]),
				(char *)get_fs_long((int *)&args[1]),
				get_fs_long((int *)&args[2]));
	case SYS_SHUTDOWN:
		return sys_shutdown(get_fs_long((int *)&args[0]),
				    get_fs_long((int *)&args[1]));
	}
	return -ENOSYS;
}
