#include <fs/fs.h>
#include <misc.h>

#define NR_FILE	1024

static struct file all[NR_FILE];

static spinlock_t file_lock;

void file_init(void)
{
	INIT_LOCK(&file_lock);
}

struct file *get_empty_file(void)
{
	int i;
	struct file *res = NULL;

	ACQUIRE_LOCK(&file_lock);
	for (i = 0; i < NR_FILE; i++)
		if (!all[i].f_count) {
			res = all + i;
			res->f_pos = 0;
			res->f_count = 1;
			res->f_inode = NULL;
			res->f_op = NULL;
			res->private_data = NULL;
			break;
		}
	RELEASE_LOCK(&file_lock);
	return res;
}

int get_two_empty_files(struct file *res[2])
{
	int i;
	int j;

	ACQUIRE_LOCK(&file_lock);
	for (i = 0, j = 0; i < NR_FILE && j < 2; ++i) {
		if (!all[i].f_count) {
			res[j] = all + i;
			++j;
		}
	}
	if (j == 2) {
		for (j = 0; j < 2; ++j) {
			res[j]->f_pos = 0;
			res[j]->f_count = 1;
			res[j]->f_inode = NULL;
			res[j]->f_op = NULL;
			res[j]->private_data = NULL;
		}
	}
	RELEASE_LOCK(&file_lock);

	if (j == 2)
		return 0;
	return 1;
}

void release_file(struct file *f, int release)
{
	struct inode *i;

	ACQUIRE_LOCK(&file_lock);
	assert(f->f_count > 0);

	if (f->f_count > 1) {
		f->f_count--;
		RELEASE_LOCK(&file_lock);
		return;
	}

	/*
	 * Keep f_count at 1 while the filesystem release method runs so another
	 * CPU cannot observe a free slot and reuse this struct file.
	 */
	i = f->f_inode;
	RELEASE_LOCK(&file_lock);

	if (release && f->f_op && f->f_op->release && i)
		f->f_op->release(i, f);

	ACQUIRE_LOCK(&file_lock);
	f->f_count--;
	f->f_inode = NULL;
	RELEASE_LOCK(&file_lock);

	if (i)
		iput(i);
}

void put_file(struct file *f)
{
	release_file(f, 0);
}

/*
 * Just want to feel safe
 * Increase reference count of a file by 1
 * This function should be called when reference count of that file is not 0
 */
void fref(struct file *f)
{
	ACQUIRE_LOCK(&file_lock);
	f->f_count++;
	RELEASE_LOCK(&file_lock);
}

struct file *get_fd_file(unsigned int fd, const struct file_operations *ops)
{
	struct file *file;

	if (fd >= NR_OPEN)
		return NULL;
	ACQUIRE_LOCK(&file_lock);
	file = current->files->fd[fd];
	if (file && (!ops || file->f_op == ops))
		file->f_count++;
	else
		file = NULL;
	RELEASE_LOCK(&file_lock);
	return file;
}

int install_fd_file(struct file *file, int close_on_exec)
{
	struct files_struct *files = current->files;
	int fd;

	ACQUIRE_LOCK(&file_lock);
	for (fd = 0; fd < NR_OPEN && files->fd[fd]; fd++)
		;
	if (fd < NR_OPEN) {
		if (close_on_exec)
			FD_SET(fd, &files->close_on_exec);
		else
			FD_CLR(fd, &files->close_on_exec);
		files->fd[fd] = file;
	}
	RELEASE_LOCK(&file_lock);
	return fd < NR_OPEN ? fd : -EMFILE;
}

int dup_fd_file(unsigned int old_fd, unsigned int target, int exact, int close_on_exec)
{
	struct files_struct *files = current->files;
	struct file *replaced = NULL;
	struct file *file;
	int result;

	if (old_fd >= NR_OPEN)
		return -EBADF;
	ACQUIRE_LOCK(&file_lock);
	file = files->fd[old_fd];
	if (!file) {
		result = -EBADF;
		goto out;
	}
	if (target >= NR_OPEN) {
		result = exact ? -EBADF : -EINVAL;
		goto out;
	}
	if (exact && old_fd == target) {
		result = target;
		goto out;
	}
	if (!exact) {
		while (target < NR_OPEN && files->fd[target])
			target++;
		if (target == NR_OPEN) {
			result = -EMFILE;
			goto out;
		}
	}
	replaced = files->fd[target];
	file->f_count++;
	files->fd[target] = file;
	if (close_on_exec)
		FD_SET(target, &files->close_on_exec);
	else
		FD_CLR(target, &files->close_on_exec);
	result = target;
out:
	RELEASE_LOCK(&file_lock);
	if (replaced)
		release_file(replaced, 1);
	return result;
}

void file_set_nonblock(struct file *file, int enabled)
{
	unsigned short flags;

	ACQUIRE_LOCK(&file_lock);
	flags = (file->f_flags & ~O_NONBLOCK) | (enabled ? O_NONBLOCK : 0);
	__atomic_store_n(&file->f_flags, flags, __ATOMIC_RELAXED);
	RELEASE_LOCK(&file_lock);
}

int sys_fcntl(unsigned int fd, unsigned int cmd, unsigned long arg)
{
	struct files_struct *files = current->files;
	struct file *file;
	int result = 0;

	if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC)
		return dup_fd_file(fd, arg, 0, cmd == F_DUPFD_CLOEXEC);
	if (fd >= NR_OPEN)
		return -EBADF;
	ACQUIRE_LOCK(&file_lock);
	file = files->fd[fd];
	if (!file) {
		result = -EBADF;
		goto out;
	}
	switch (cmd) {
	case F_GETFD:
		result = FD_ISSET(fd, &files->close_on_exec);
		break;
	case F_SETFD:
		if (arg & 1)
			FD_SET(fd, &files->close_on_exec);
		else
			FD_CLR(fd, &files->close_on_exec);
		break;
	case F_GETFL:
		result = file->f_flags;
		break;
	case F_SETFL: {
		unsigned short flags = (file->f_flags & ~(O_APPEND | O_NONBLOCK)) |
			(arg & (O_APPEND | O_NONBLOCK));

		__atomic_store_n(&file->f_flags, flags, __ATOMIC_RELAXED);
		break;
	}
	default:
		result = -EINVAL;
		break;
	}
out:
	RELEASE_LOCK(&file_lock);
	return result;
}

int sys_close(unsigned int fd)
{
	struct files_struct *files = current->files;
	struct file *file;

	if (fd >= NR_OPEN)
		return -EBADF;
	ACQUIRE_LOCK(&file_lock);
	file = files->fd[fd];
	files->fd[fd] = NULL;
	FD_CLR(fd, &files->close_on_exec);
	RELEASE_LOCK(&file_lock);
	if (!file)
		return -EBADF;
	release_file(file, 1);
	return 0;
}

static struct file *copy_fd(struct file *old_file)
{
	int error;
	struct file *new_file;

	if (!(new_file = get_empty_file()))
		return NULL;
	*new_file = *old_file;
	new_file->f_count = 1;

	struct inode *inode = new_file->f_inode;
	if (inode) {
		iref(inode);

		/*
		 * Duplicated pipe/FIFO descriptors must keep the inode's endpoint
		 * counts accurate.  These counts are shared across CPUs and are
		 * also touched by pipe_read_release()/pipe_write_release(), so
		 * take the pipe lock here rather than doing an unlocked ++.  On
		 * SMP an unlocked increment can race with a close and make a pipe
		 * appear closed before its other endpoint has exited.
		 */
		if (S_ISFIFO(inode->i_mode)) {
			struct pipe_inode_info *info = &inode->u.pipe_i;

			ACQUIRE_LOCK(&info->lock);
			if (new_file->f_mode & 1)
				info->n_reader++;
			if (new_file->f_mode & 2)
				info->n_writer++;
			RELEASE_LOCK(&info->lock);
		}
	}

	if (new_file->f_op && new_file->f_op->open) {
		error = new_file->f_op->open(inode, new_file);
		if (error) {
			put_file(new_file);
			return NULL;
		}
	}
	return new_file;
}

void copy_files(struct task_struct *p)
{
	int i;
	struct file *f;

	ACQUIRE_LOCK(&file_lock);
	*(p->files) = *(current->files);
	for (i = 0; i < NR_OPEN; i++)
		if ((f = p->files->fd[i]))
			f->f_count++;
	RELEASE_LOCK(&file_lock);

	for (i = 0; i < NR_OPEN; i++) {
		f = p->files->fd[i];
		if (!f || (f->f_op && f->f_op->share_on_fork))
			continue;
		p->files->fd[i] = copy_fd(f);
		release_file(f, 1);
	}
}

void copy_fs(struct task_struct *p)
{
	p->fs->root = current->fs->root;
	p->fs->pwd = current->fs->pwd;
	p->fs->umask = __atomic_load_n(&current->fs->umask, __ATOMIC_RELAXED);

	if (p->fs->pwd)
		iref(p->fs->pwd);
	if (p->fs->root)
		iref(p->fs->root);
}

void exit_files(void)
{
	int i;

	for (i = 0; i < NR_OPEN; i++)
		if (current->files->fd[i])
			sys_close(i);
}

void exit_fs(void)
{
	if (current->fs->pwd) {
		iput(current->fs->pwd);
		current->fs->pwd = NULL;
	}
	if (current->fs->root) {
		iput(current->fs->root);
		current->fs->root = NULL;
	}
}
