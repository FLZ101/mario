#include <fs/pipe.h>
#include <fs/fs.h>

int init_pipe_inode_info(struct pipe_inode_info *info)
{
	int error;

	if ((error = ring_buffer_init(&info->rb)))
		return error;
	init_wait_queue(&info->wait_read);
	init_wait_queue(&info->wait_write);
	info->n_reader = 1;
	info->n_writer = 1;
	INIT_LOCK(&info->lock);
	return 0;
}

void destroy_pipe_inode_info(struct pipe_inode_info *info)
{
	ring_buffer_destroy(&info->rb);
	info->n_reader = 0;
	info->n_writer = 0;
}

static void pipe_read_release(struct inode *i, struct file *f)
{
	struct pipe_inode_info *info = &i->u.pipe_i;

	ACQUIRE_LOCK(&info->lock);

	assert(info->n_reader > 0);
	info->n_reader--;
	wake_up_interruptible(&info->wait_write);

	RELEASE_LOCK(&info->lock);
}

static void pipe_write_release(struct inode *i, struct file *f)
{
	struct pipe_inode_info *info = &i->u.pipe_i;

	ACQUIRE_LOCK(&info->lock);

	assert(info->n_writer > 0);
	info->n_writer--;
	wake_up_interruptible(&info->wait_read);

	RELEASE_LOCK(&info->lock);
}

int pipe_write(struct inode *i, struct file *f, char *buf, int count)
{
	int error;
	struct pipe_inode_info *info = &i->u.pipe_i;

try:
	if (current->signal & ~current->blocked)
		return -ERESTARTSYS;

	ACQUIRE_LOCK(&info->lock);
	if (!info->n_reader) {
		send_sig(SIGPIPE, current, 0);
		error = -EPIPE;
		goto tail;
	}
	if (ring_buffer_full(&info->rb)) {
		if (f->f_flags & O_NONBLOCK) {
			error = -EAGAIN;
			goto tail;
		}

		sleep_on(&info->wait_write, TASK_INTERRUPTIBLE, &info->lock);
		goto try;
	} else {
		error = ring_buffer_write(&info->rb, buf, count, 1);
		wake_up_interruptible(&info->wait_read);
	}
tail:
	RELEASE_LOCK(&info->lock);
	return error;
}

int pipe_read(struct inode *i, struct file *f, char *buf, int count)
{
	int error;
	struct pipe_inode_info *info = &i->u.pipe_i;

try:
	if (current->signal & ~current->blocked)
		return -ERESTARTSYS;

	ACQUIRE_LOCK(&info->lock);
	if (ring_buffer_empty(&info->rb)) {
		if (!info->n_writer) {
			error = 0;
			goto tail;
		}

		if (f->f_flags & O_NONBLOCK) {
			error = -EAGAIN;
			goto tail;
		}

		sleep_on(&info->wait_read, TASK_INTERRUPTIBLE, &info->lock);
		goto try;
	} else {
		error = ring_buffer_read(&info->rb, buf, count, 1);
		wake_up_interruptible(&info->wait_write);
	}
tail:
	RELEASE_LOCK(&info->lock);
	return error;
}

static int pipe_lseek(struct inode *i, struct file *f, off_t offset, int origin)
{
	return -ESPIPE;
}

static int bad_pipe_rw(struct inode *i, struct file *f, char *buf, int count)
{
	return -EBADF;
}

static int pipe_read_poll(struct file *f, struct poll_context *ctx)
{
	struct inode *i = f->f_inode;
	assert(i);
	struct pipe_inode_info *info = &i->u.pipe_i;

	poll_wait(f, &info->wait_read, ctx);

	ACQUIRE_LOCK(&info->lock);
	short mask = 0;
	if (!ring_buffer_empty(&info->rb))
		mask |= POLLIN;
	else if (!info->n_writer)
		mask |= POLLHUP;
	RELEASE_LOCK(&info->lock);
	return mask;
}

static int pipe_write_poll(struct file *f, struct poll_context *ctx)
{
	struct inode *i = f->f_inode;
	assert(i);
	struct pipe_inode_info *info = &i->u.pipe_i;

	poll_wait(f, &info->wait_write, ctx);

	ACQUIRE_LOCK(&info->lock);
	short mask = 0;
	if (!info->n_reader) {
		mask |= POLLHUP;
	} else if (!ring_buffer_full(&info->rb))
		mask |= POLLOUT;
	RELEASE_LOCK(&info->lock);
	return mask;
}

struct file_operations read_pipe_fops = {
	.release = pipe_read_release,
	.lseek = pipe_lseek,
	.read = pipe_read,
	.write = bad_pipe_rw,
	.poll = pipe_read_poll,
};

struct file_operations write_pipe_fops = {
	.release = pipe_write_release,
	.lseek = pipe_lseek,
	.read = bad_pipe_rw,
	.write = pipe_write,
	.poll = pipe_write_poll,
};

/* A read-write FIFO descriptor owns both the reader and writer endpoints. */
static void fifo_rdwr_release(struct inode *i, struct file *f)
{
	struct pipe_inode_info *info = &i->u.pipe_i;

	ACQUIRE_LOCK(&info->lock);
	if (info->n_reader)
		info->n_reader--;
	if (info->n_writer)
		info->n_writer--;
	wake_up_interruptible(&info->wait_read);
	wake_up_interruptible(&info->wait_write);
	RELEASE_LOCK(&info->lock);
}

static struct file_operations rdwr_pipe_fops = {
	.release = fifo_rdwr_release,
	.lseek = pipe_lseek,
	.read = pipe_read,
	.write = pipe_write,
};

/*
 * Attach an opened MarioFS FIFO to its shared per-inode pipe.  Read-only and
 * write-only opens wait for the opposite side unless O_NONBLOCK is set, while
 * an O_RDWR open provides both sides and can proceed immediately.
 */
int fifo_open(struct inode *i, struct file *f)
{
	int error = 0;
	struct pipe_inode_info *info = &i->u.pipe_i;

	/* The first opener allocates the backing ring buffer. */
	if (!inode_pipe(i)) {
		error = init_pipe_inode_info(info);
		if (error)
			return error;
		info->n_reader = 0;
		info->n_writer = 0;
		set_bit(I_Pipe, &i->i_state);
	}

	ACQUIRE_LOCK(&info->lock);

	if ((f->f_mode & 1) && (f->f_mode & 2)) {
		info->n_reader++;
		info->n_writer++;
		f->f_op = &rdwr_pipe_fops;
		wake_up_interruptible(&info->wait_read);
		wake_up_interruptible(&info->wait_write);
		RELEASE_LOCK(&info->lock);
		return 0;
	}

	if (f->f_mode & 1) {
		info->n_reader++;
		f->f_op = &read_pipe_fops;
		/* Let any writer waiting for a reader continue. */
		wake_up_interruptible(&info->wait_write);
		if (!info->n_writer && !(f->f_flags & O_NONBLOCK)) {
			/*
			 * Sleep exactly once.  The wake-up means a writer existed,
			 * even if it already closed before this reader runs again.
			 * Rechecking n_writer here would turn that valid rendezvous
			 * into a lost wake-up and deadlock the open.
			 */
			sleep_on(&info->wait_read, TASK_INTERRUPTIBLE, &info->lock);
			ACQUIRE_LOCK(&info->lock);
			if (current->signal & ~current->blocked) {
				info->n_reader--;
				wake_up_interruptible(&info->wait_write);
				error = -ERESTARTSYS;
			}
		}
		RELEASE_LOCK(&info->lock);
		return error;
	}

	info->n_writer++;
	f->f_op = &write_pipe_fops;
	/* Let any reader waiting for a writer continue. */
	wake_up_interruptible(&info->wait_read);
	if (!info->n_reader) {
		if (f->f_flags & O_NONBLOCK) {
			info->n_writer--;
			error = -ENXIO;
		} else {
			/*
			 * As with read opens, a single wake-up is enough to complete
			 * a blocking write open: the partner may have opened and then
			 * closed before this task was scheduled again.
			 */
			sleep_on(&info->wait_write, TASK_INTERRUPTIBLE, &info->lock);
			ACQUIRE_LOCK(&info->lock);
			if (current->signal & ~current->blocked) {
				info->n_writer--;
				wake_up_interruptible(&info->wait_read);
				error = -ERESTARTSYS;
			}
		}
	}
	RELEASE_LOCK(&info->lock);
	return error;
}

struct file_operations fifo_fops = {
	.open = fifo_open,
};

int do_pipe(int pipefd[2]) {
	int error;
	struct inode *i;
	struct file *f[2];
	int j;
	int fd;

	if (!(i = get_pipe_inode()))
		return -ENFILE;

	if ((error = init_pipe_inode_info(&i->u.pipe_i)))
		goto fail_1;

	if (get_two_empty_files(f)) {
		error = -ENFILE;
		goto fail_1;
	}

	for (fd = 0, j = 0; j < 2; fd++) {
		if (fd == NR_OPEN)
			return -EMFILE;
		if (!current->files->fd[fd]) {
			pipefd[j] = fd;
			++j;
		}
	}
	if (j < 2) {
		error = -ENFILE;
		goto fail_2;
	}

	current->files->fd[pipefd[0]] = f[0];
	current->files->fd[pipefd[1]] = f[1];

	f[0]->f_inode = f[1]->f_inode = i;
	f[0]->f_pos = f[1]->f_pos = 0;
	f[0]->f_flags = O_RDONLY;
	f[0]->f_op = &read_pipe_fops;
	f[0]->f_mode = 1;		/* read */
	f[1]->f_flags = O_WRONLY;
	f[1]->f_op = &write_pipe_fops;
	f[1]->f_mode = 2;		/* write */

	i->i_count = 2;
	return 0;

fail_2:
	for (j = 0; j < 2; ++j) {
		put_file(f[j]);
	}

fail_1:
	iput(i);
	return error;
}

int sys_pipe(unsigned long *pipefd)
{
	int fd[2];
	int error;

	error = verify_area(VERIFY_WRITE, pipefd, 2 * sizeof(long));
	if (error)
		return error;
	error = do_pipe(fd);
	if (error)
		return error;
	put_fs_long(fd[0], pipefd+0);
	put_fs_long(fd[1], pipefd+1);
	return 0;
}

int sys_pipe2(unsigned long *pipefd, int flags)
{
	int fd[2];
	int error;

	if (flags & ~(O_CLOEXEC | O_NONBLOCK))
		return -EINVAL;

	error = verify_area(VERIFY_WRITE, pipefd, 2 * sizeof(long));
	if (error)
		return error;
	error = do_pipe(fd);
	if (error)
		return error;

	if (flags & O_NONBLOCK) {
		current->files->fd[fd[0]]->f_flags |= O_NONBLOCK;
		current->files->fd[fd[1]]->f_flags |= O_NONBLOCK;
	}
	if (flags & O_CLOEXEC) {
		FD_SET(fd[0], &current->files->close_on_exec);
		FD_SET(fd[1], &current->files->close_on_exec);
	}

	put_fs_long(fd[0], pipefd + 0);
	put_fs_long(fd[1], pipefd + 1);
	return 0;
}
