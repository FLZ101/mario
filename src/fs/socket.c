#include <fs/socket.h>
#include <mm/kmalloc.h>
#include <misc.h>

#define SYS_SOCKET 1
#define SYS_BIND 2
#define SYS_CONNECT 3
#define SYS_LISTEN 4
#define SYS_ACCEPT 5
#define SYS_SOCKETPAIR 8
#define SYS_SEND 9
#define SYS_RECV 10
#define SYS_SENDTO 11
#define SYS_RECVFROM 12
#define SYS_SHUTDOWN 13
#define SYS_GETSOCKOPT 15
#define SYS_SENDMSG 16
#define SYS_RECVMSG 17

#define UNIX_PATH_MAX 108
#define UNIX_LISTENER_MAX 32
#define UNIX_RIGHTS_MAX 16
#define UNIX_IOV_MAX 1024
#define SOL_SOCKET 1
#define SCM_RIGHTS 1
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_ACCEPTCONN 30
#define SO_PEERCRED 17

extern int sys_read(unsigned int fd, char *buf, unsigned int count);
extern int sys_write(unsigned int fd, char *buf, unsigned int count);

struct unix_socket;

struct unix_socket_pair {
	struct unix_socket *endpoint[2];
	spinlock_t lock;
};

struct unix_socket {
	struct unix_socket_pair *pair;
	struct ring_buffer receive;
	wait_queue_t wait_read;
	wait_queue_t wait_write;
	wait_queue_t wait_accept;
	struct unix_socket *pending_next;
	struct unix_socket *pending_head;
	struct unix_socket *pending_tail;
	int opens;
	int read_shutdown;
	int write_shutdown;
	int bound;
	int listening;
	struct file *rights[UNIX_RIGHTS_MAX];
	unsigned int rights_head;
	unsigned int rights_tail;
	unsigned int rights_count;
	char path[UNIX_PATH_MAX];
};

struct unix_sockaddr {
	unsigned short family;
	char path[UNIX_PATH_MAX];
};

struct socket_iovec {
	void *base;
	unsigned int len;
};

struct socket_msghdr {
	void *name;
	unsigned int namelen;
	struct socket_iovec *iov;
	int iovlen;
	void *control;
	unsigned int controllen;
	int flags;
};

struct socket_cmsghdr {
	unsigned int len;
	int level;
	int type;
};

struct socket_ucred {
	int pid;
	unsigned int uid;
	unsigned int gid;
};

static struct unix_socket *unix_listeners[UNIX_LISTENER_MAX];
static spinlock_t unix_lock;

static struct unix_socket *socket_endpoint(struct file *f)
{
	return f->private_data;
}

static struct unix_socket *socket_peer(struct unix_socket *socket)
{
	struct unix_socket_pair *pair = socket->pair;

	return pair->endpoint[socket == pair->endpoint[0]];
}

static void socket_destroy(struct unix_socket *socket)
{
	while (socket->rights_count) {
		struct file *file = socket->rights[socket->rights_head];

		socket->rights_head =
			(socket->rights_head + 1) % UNIX_RIGHTS_MAX;
		socket->rights_count--;
		release_file(file, 1);
	}
	ring_buffer_destroy(&socket->receive);
	kfree(socket);
}

static struct unix_socket *socket_alloc(void)
{
	struct unix_socket *socket = kmalloc(sizeof(*socket));

	if (!socket)
		return NULL;
	memset(socket, 0, sizeof(*socket));
	if (ring_buffer_init(&socket->receive)) {
		kfree(socket);
		return NULL;
	}
	init_wait_queue(&socket->wait_read);
	init_wait_queue(&socket->wait_write);
	init_wait_queue(&socket->wait_accept);
	socket->opens = 1;
	return socket;
}

static struct unix_socket_pair *socket_pair_alloc(struct unix_socket *first,
						    struct unix_socket *second)
{
	struct unix_socket_pair *pair = kmalloc(sizeof(*pair));

	if (!pair)
		return NULL;
	memset(pair, 0, sizeof(*pair));
	INIT_LOCK(&pair->lock);
	pair->endpoint[0] = first;
	pair->endpoint[1] = second;
	first->pair = pair;
	second->pair = pair;
	return pair;
}

static int socket_read(struct inode *i, struct file *f, char *buf, int count)
{
	struct unix_socket *socket = socket_endpoint(f);
	struct unix_socket *peer;
	unsigned long flags;
	int n;

	if (!socket->pair)
		return -ENOTCONN;
	peer = socket_peer(socket);
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
	struct unix_socket *peer;
	unsigned long flags;
	int n;

	if (!socket->pair)
		return -ENOTCONN;
	peer = socket_peer(socket);
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

static int socket_queue_right(struct unix_socket *socket, struct file *file)
{
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&socket->pair->lock, flags);
	if (socket->rights_count == UNIX_RIGHTS_MAX)
		err = -EAGAIN;
	else {
		fref(file);
		socket->rights[socket->rights_tail] = file;
		socket->rights_tail =
			(socket->rights_tail + 1) % UNIX_RIGHTS_MAX;
		socket->rights_count++;
	}
	spin_unlock_irqrestore(&socket->pair->lock, flags);
	return err;
}

static struct file *socket_take_right(struct unix_socket *socket)
{
	struct file *file = NULL;
	unsigned long flags;

	spin_lock_irqsave(&socket->pair->lock, flags);
	if (socket->rights_count) {
		file = socket->rights[socket->rights_head];
		socket->rights_head =
			(socket->rights_head + 1) % UNIX_RIGHTS_MAX;
		socket->rights_count--;
	}
	spin_unlock_irqrestore(&socket->pair->lock, flags);
	return file;
}

static int socket_lseek(struct inode *i, struct file *f, off_t offset, int origin)
{
	return -ESPIPE;
}

static int socket_poll(struct file *f, struct poll_context *ctx)
{
	struct unix_socket *socket = socket_endpoint(f);
	struct unix_socket *peer;
	unsigned long flags;
	short mask = 0;

	if (socket->listening) {
		poll_wait(f, &socket->wait_accept, ctx);
		spin_lock_irqsave(&unix_lock, flags);
		if (socket->pending_head)
			mask |= POLLIN;
		spin_unlock_irqrestore(&unix_lock, flags);
		return mask;
	}
	if (!socket->pair)
		return POLLERR;
	peer = socket_peer(socket);
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
	struct unix_socket *peer;
	struct unix_socket_pair *pair;
	unsigned long flags;
	int free_pair = 0;

	if (!socket->pair) {
		spin_lock_irqsave(&unix_lock, flags);
		assert(socket->opens > 0);
		if (!--socket->opens) {
			for (int index = 0; index < UNIX_LISTENER_MAX; index++) {
				if (unix_listeners[index] == socket)
					unix_listeners[index] = NULL;
			}
			socket->bound = 0;
			wake_up_interruptible(&socket->wait_accept);
		}
		spin_unlock_irqrestore(&unix_lock, flags);
		if (!socket->opens)
			socket_destroy(socket);
		return;
	}

	pair = socket->pair;
	peer = socket_peer(socket);
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
		socket_destroy(pair->endpoint[0]);
		socket_destroy(pair->endpoint[1]);
		kfree(pair);
	}
}

static int socket_open(struct inode *i, struct file *f)
{
	struct unix_socket *socket = socket_endpoint(f);
	unsigned long flags;

	if (socket->pair) {
		spin_lock_irqsave(&socket->pair->lock, flags);
		socket->opens++;
		spin_unlock_irqrestore(&socket->pair->lock, flags);
	} else {
		spin_lock_irqsave(&unix_lock, flags);
		socket->opens++;
		spin_unlock_irqrestore(&unix_lock, flags);
	}
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

static int socket_install(struct unix_socket *socket, int type, int fd)
{
	struct file *file;
	struct inode *inode;

	file = get_empty_file();
	if (!file)
		return -ENFILE;
	inode = get_socket_inode();
	if (!inode) {
		put_file(file);
		return -ENFILE;
	}
	file->f_inode = inode;
	file->f_op = &socket_fops;
	file->private_data = socket;
	file->f_mode = 3;
	file->f_flags = O_RDWR | (type & (SOCK_NONBLOCK | SOCK_CLOEXEC));
	if (type & SOCK_CLOEXEC)
		FD_SET(fd, &current->files->close_on_exec);
	current->files->fd[fd] = file;
	return 0;
}

static int socket_address(const void *address, unsigned int length,
			  char path[UNIX_PATH_MAX])
{
	struct unix_sockaddr addr;
	int path_len;

	if (length < sizeof(addr.family) || length > sizeof(addr))
		return -EINVAL;
	if (verify_area(VERIFY_READ, (void *)address, length))
		return -EFAULT;
	memset(&addr, 0, sizeof(addr));
	memcpy_fromfs(&addr, address, length);
	if (addr.family != AF_UNIX)
		return -EAFNOSUPPORT;
	path_len = length - sizeof(addr.family);
	for (int index = 0; index < path_len; index++) {
		if (!addr.path[index]) {
			memcpy(path, addr.path, index + 1);
			return index ? 0 : -EINVAL;
		}
	}
	return -ENAMETOOLONG;
}

static struct unix_socket *socket_find(const char *path)
{
	for (int index = 0; index < UNIX_LISTENER_MAX; index++) {
		if (unix_listeners[index] && !strcmp(unix_listeners[index]->path, path))
			return unix_listeners[index];
	}
	return NULL;
}

int sys_socket(int domain, int type, int protocol)
{
	struct unix_socket *socket;
	int fd;
	int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	int err;

	if (domain != AF_UNIX || base_type != SOCK_STREAM || protocol)
		return -EOPNOTSUPP;
	fd = next_fd(0);
	if (fd < 0)
		return fd;
	socket = socket_alloc();
	if (!socket)
		return -ENOMEM;
	err = socket_install(socket, type, fd);
	if (err) {
		socket_destroy(socket);
		return err;
	}
	return fd;
}

int do_socketpair(int domain, int type, int protocol, int fd[2])
{
	struct unix_socket_pair *pair;
	struct unix_socket *socket[2];
	int base_type = type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	int err;

	if (domain != AF_UNIX || base_type != SOCK_STREAM || protocol)
		return -EOPNOTSUPP;
	fd[0] = next_fd(0);
	if (fd[0] < 0)
		return fd[0];
	fd[1] = next_fd(fd[0] + 1);
	if (fd[1] < 0)
		return fd[1];
	socket[0] = socket_alloc();
	socket[1] = socket_alloc();
	if (!socket[0] || !socket[1]) {
		if (socket[0])
			socket_destroy(socket[0]);
		if (socket[1])
			socket_destroy(socket[1]);
		return -ENOMEM;
	}
	pair = socket_pair_alloc(socket[0], socket[1]);
	if (!pair) {
		socket_destroy(socket[0]);
		socket_destroy(socket[1]);
		return -ENOMEM;
	}
	err = socket_install(socket[0], type, fd[0]);
	if (!err)
		err = socket_install(socket[1], type, fd[1]);
	if (!err)
		return 0;
	if (current->files->fd[fd[0]])
		release_file(current->files->fd[fd[0]], 1);
	if (current->files->fd[fd[1]])
		release_file(current->files->fd[fd[1]], 1);
	if (!current->files->fd[fd[0]] && !current->files->fd[fd[1]]) {
		socket_destroy(socket[0]);
		socket_destroy(socket[1]);
		kfree(pair);
	}
	return err;
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

int sys_bind(int fd, const void *address, unsigned int length)
{
	struct file *file;
	struct unix_socket *socket;
	char path[UNIX_PATH_MAX];
	unsigned long flags;
	int err;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	socket = socket_endpoint(file);
	if (socket->pair || socket->bound)
		return -EINVAL;
	err = socket_address(address, length, path);
	if (err)
		return err;
	spin_lock_irqsave(&unix_lock, flags);
	if (socket_find(path)) {
		spin_unlock_irqrestore(&unix_lock, flags);
		return -EADDRINUSE;
	}
	for (int index = 0; index < UNIX_LISTENER_MAX; index++) {
		if (!unix_listeners[index]) {
			unix_listeners[index] = socket;
			strcpy(socket->path, path);
			socket->bound = 1;
			spin_unlock_irqrestore(&unix_lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&unix_lock, flags);
	return -ENFILE;
}

int sys_listen(int fd, int backlog)
{
	struct file *file;
	struct unix_socket *socket;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	socket = socket_endpoint(file);
	if (!socket->bound || socket->pair)
		return -EINVAL;
	socket->listening = 1;
	return 0;
}

int sys_connect(int fd, const void *address, unsigned int length)
{
	struct file *file;
	struct unix_socket *client;
	struct unix_socket *server;
	struct unix_socket *listener;
	struct unix_socket_pair *pair;
	char path[UNIX_PATH_MAX];
	unsigned long flags;
	int err;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	client = socket_endpoint(file);
	if (client->pair)
		return -EISCONN;
	err = socket_address(address, length, path);
	if (err)
		return err;
	server = socket_alloc();
	if (!server)
		return -ENOMEM;
	pair = socket_pair_alloc(client, server);
	if (!pair) {
		socket_destroy(server);
		return -ENOMEM;
	}
	spin_lock_irqsave(&unix_lock, flags);
	listener = socket_find(path);
	if (!listener || !listener->listening || !listener->opens) {
		spin_unlock_irqrestore(&unix_lock, flags);
		client->pair = NULL;
		server->pair = NULL;
		socket_destroy(server);
		kfree(pair);
		return listener ? -ECONNREFUSED : -ENOENT;
	}
	if (listener->pending_tail)
		listener->pending_tail->pending_next = server;
	else
		listener->pending_head = server;
	listener->pending_tail = server;
	spin_unlock_irqrestore(&unix_lock, flags);
	wake_up_interruptible(&listener->wait_accept);
	return 0;
}

int sys_accept4(int fd, void *address, unsigned int *length, int flags)
{
	struct file *file;
	struct unix_socket *listener;
	struct unix_socket *socket;
	unsigned long irq_flags;
	int newfd;
	int err;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	listener = socket_endpoint(file);
	if (!listener->listening)
		return -EINVAL;
	if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
		return -EINVAL;
	newfd = next_fd(0);
	if (newfd < 0)
		return newfd;
	for (;;) {
		spin_lock_irqsave(&unix_lock, irq_flags);
		socket = listener->pending_head;
		if (socket) {
			listener->pending_head = socket->pending_next;
			if (!listener->pending_head)
				listener->pending_tail = NULL;
			socket->pending_next = NULL;
			spin_unlock_irqrestore(&unix_lock, irq_flags);
			break;
		}
		spin_unlock_irqrestore(&unix_lock, irq_flags);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		if (current->signal & ~current->blocked)
			return -ERESTARTSYS;
		sleep_on(&listener->wait_accept, TASK_INTERRUPTIBLE, NULL);
	}
	err = socket_install(socket, flags, newfd);
	if (err)
		return err;
	return newfd;
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
	if (!socket->pair)
		return -ENOTCONN;
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

int sys_getsockopt(int fd, int level, int option, void *value,
		   unsigned int *length)
{
	struct file *file;
	struct unix_socket *socket;
	struct socket_ucred cred;
	unsigned int user_length;
	int result;
	unsigned int result_length;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	if (level != SOL_SOCKET || verify_area(VERIFY_READ, length, sizeof(*length)))
		return -EINVAL;
	user_length = get_fs_long((int *)length);
	socket = socket_endpoint(file);
	if (option == SO_PEERCRED) {
		cred.pid = 0;
		cred.uid = 0;
		cred.gid = 0;
		result_length = sizeof(cred);
		if (user_length < result_length ||
		    verify_area(VERIFY_WRITE, value, result_length))
			return -EINVAL;
		memcpy_tofs(value, &cred, result_length);
	} else {
		if (option == SO_TYPE)
			result = SOCK_STREAM;
		else if (option == SO_ERROR)
			result = 0;
		else if (option == SO_ACCEPTCONN)
			result = socket->listening;
		else
			return -ENOPROTOOPT;
		result_length = sizeof(result);
		if (user_length < result_length ||
		    verify_area(VERIFY_WRITE, value, result_length))
			return -EINVAL;
		put_fs_long(result, (int *)value);
	}
	put_fs_long(result_length, (int *)length);
	return 0;
}

static int socket_send_right(struct socket_msghdr *msg, struct file **file)
{
	struct socket_cmsghdr cmsg;
	int fd;

	*file = NULL;
	if (!msg->controllen)
		return 0;
	if (msg->controllen < sizeof(cmsg) + sizeof(fd) ||
	    verify_area(VERIFY_READ, msg->control, sizeof(cmsg) + sizeof(fd)))
		return -EINVAL;
	memcpy_fromfs(&cmsg, msg->control, sizeof(cmsg));
	if (cmsg.len < sizeof(cmsg) + sizeof(fd) ||
	    cmsg.level != SOL_SOCKET || cmsg.type != SCM_RIGHTS)
		return -EINVAL;
	fd = get_fs_long((int *)(msg->control + sizeof(cmsg)));
	if (fd < 0 || fd >= NR_OPEN || !current->files->fd[fd])
		return -EBADF;
	*file = current->files->fd[fd];
	return 0;
}

int sys_sendmsg(int fd, struct socket_msghdr *user_msg, int flags)
{
	struct socket_msghdr msg;
	struct socket_iovec iov;
	struct file *right;
	struct file *file;
	struct unix_socket *socket;
	int total = 0;
	int err;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	if (verify_area(VERIFY_READ, user_msg, sizeof(msg)))
		return -EFAULT;
	memcpy_fromfs(&msg, user_msg, sizeof(msg));
	if (msg.iovlen < 0 || msg.iovlen > UNIX_IOV_MAX ||
	    (msg.iovlen && verify_area(VERIFY_READ, msg.iov,
					msg.iovlen * sizeof(iov))))
		return -EINVAL;
	err = socket_send_right(&msg, &right);
	if (err)
		return err;
	for (int index = 0; index < msg.iovlen; index++) {
		memcpy_fromfs(&iov, &msg.iov[index], sizeof(iov));
		if (!iov.len)
			continue;
		err = sys_write(fd, iov.base, iov.len);
		if (err < 0)
			return total ? total : err;
		total += err;
		if ((unsigned int)err < iov.len)
			break;
	}
	if (!total || !right)
		return total;
	socket = socket_endpoint(file);
	err = socket_queue_right(socket_peer(socket), right);
	if (err)
		return err;
	return total;
}

int sys_recvmsg(int fd, struct socket_msghdr *user_msg, int flags)
{
	struct socket_msghdr msg;
	struct socket_iovec iov;
	struct socket_cmsghdr cmsg;
	struct file *file;
	struct file *right;
	struct unix_socket *socket;
	int total = 0;
	int err;
	int newfd;
	unsigned int control_length;

	if (fd < 0 || fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (file->f_op != &socket_fops)
		return -ENOTSOCK;
	if (verify_area(VERIFY_READ, user_msg, sizeof(msg)))
		return -EFAULT;
	memcpy_fromfs(&msg, user_msg, sizeof(msg));
	if (msg.iovlen < 0 || msg.iovlen > UNIX_IOV_MAX ||
	    (msg.iovlen && verify_area(VERIFY_READ, msg.iov,
					msg.iovlen * sizeof(iov))))
		return -EINVAL;
	for (int index = 0; index < msg.iovlen; index++) {
		memcpy_fromfs(&iov, &msg.iov[index], sizeof(iov));
		if (!iov.len)
			continue;
		err = sys_read(fd, iov.base, iov.len);
		if (err < 0) {
			pr_err("recvmsg read fd=%d err=%d\n", fd, err);
			return total ? total : err;
		}
		total += err;
		if ((unsigned int)err < iov.len)
			break;
	}
	control_length = msg.controllen;
	msg.flags = 0;
	msg.controllen = 0;
	if (!total)
		goto done;
	socket = socket_endpoint(file);
	right = socket_take_right(socket);
	if (!right)
		goto done;
	newfd = next_fd(0);
	if (newfd < 0)
		return -EMSGSIZE;
	if (control_length < sizeof(cmsg) + sizeof(newfd) ||
	    verify_area(VERIFY_WRITE, msg.control, sizeof(cmsg) + sizeof(newfd)))
		return -EINVAL;
	cmsg.len = sizeof(cmsg) + sizeof(newfd);
	cmsg.level = SOL_SOCKET;
	cmsg.type = SCM_RIGHTS;
	memcpy_tofs(msg.control, &cmsg, sizeof(cmsg));
	put_fs_long(newfd, (int *)(msg.control + sizeof(cmsg)));
	msg.controllen = sizeof(cmsg) + sizeof(newfd);
	current->files->fd[newfd] = right;
	if (flags & 0x40000000)
		FD_SET(newfd, &current->files->close_on_exec);
done:
	memcpy_tofs(user_msg, &msg, sizeof(msg));
	return total;
}

void socket_init(void)
{
	memset(unix_listeners, 0, sizeof(unix_listeners));
	INIT_LOCK(&unix_lock);
}

int sys_socketcall(int call, unsigned long *args)
{
	if (call < SYS_SOCKET || call > SYS_RECVMSG)
		return -ENOSYS;
	if (verify_area(VERIFY_READ, args, 4 * sizeof(unsigned long)))
		return -EFAULT;
	switch (call) {
	case SYS_SOCKET:
		return sys_socket(get_fs_long((int *)&args[0]),
				  get_fs_long((int *)&args[1]),
				  get_fs_long((int *)&args[2]));
	case SYS_BIND:
		return sys_bind(get_fs_long((int *)&args[0]),
				(char *)get_fs_long((int *)&args[1]),
				get_fs_long((int *)&args[2]));
	case SYS_CONNECT:
		return sys_connect(get_fs_long((int *)&args[0]),
				   (char *)get_fs_long((int *)&args[1]),
				   get_fs_long((int *)&args[2]));
	case SYS_LISTEN:
		return sys_listen(get_fs_long((int *)&args[0]),
				  get_fs_long((int *)&args[1]));
	case SYS_ACCEPT:
		return sys_accept4(get_fs_long((int *)&args[0]),
				    (void *)get_fs_long((int *)&args[1]),
				    (unsigned int *)get_fs_long((int *)&args[2]), 0);
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
	case SYS_GETSOCKOPT:
		return sys_getsockopt(get_fs_long((int *)&args[0]),
				      get_fs_long((int *)&args[1]),
				      get_fs_long((int *)&args[2]),
				      (void *)get_fs_long((int *)&args[3]),
				      (unsigned int *)get_fs_long((int *)&args[4]));
	case SYS_SENDMSG:
		return sys_sendmsg(get_fs_long((int *)&args[0]),
				   (struct socket_msghdr *)get_fs_long((int *)&args[1]),
				   get_fs_long((int *)&args[2]));
	case SYS_RECVMSG:
		return sys_recvmsg(get_fs_long((int *)&args[0]),
				   (struct socket_msghdr *)get_fs_long((int *)&args[1]),
				   get_fs_long((int *)&args[2]));
	}
	return -ENOSYS;
}
