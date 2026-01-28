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

	info->n_reader--;
	wake_up_interruptible(&info->wait_write);
}

static void pipe_write_release(struct inode *i, struct file *f)
{
	struct pipe_inode_info *info = &i->u.pipe_i;

	info->n_writer--;
	wake_up_interruptible(&info->wait_read);
}

int pipe_write(struct inode *i, struct file *f, char *buf, int count)
{
	int error;
	struct pipe_inode_info *info = &i->u.pipe_i;

try:
	ACQUIRE_LOCK(&info->lock);
	if (ring_buffer_full(&info->rb)) {
		if (!info->n_reader) {
			send_sig(SIGPIPE, current, 0);
			error = -EPIPE;
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
	ACQUIRE_LOCK(&info->lock);
	if (ring_buffer_empty(&info->rb)) {
		if (!info->n_writer) {
			error = 0;
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

struct file_operations read_pipe_fops = {
	.release = pipe_read_release,
	.lseek = pipe_lseek,
	.read = pipe_read,
	.write = bad_pipe_rw,
};

struct file_operations write_pipe_fops = {
	.release = pipe_write_release,
	.lseek = pipe_lseek,
	.read = bad_pipe_rw,
	.write = pipe_write,
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
