#include <fs/fs.h>
#include <misc.h>

#define NR_FILE	1024

static struct file all[NR_FILE];

static spinlock_t file_lock;

void __tinit file_init(void)
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
			break;
		}
	RELEASE_LOCK(&file_lock);
	return res;
}

void put_file(struct file *f)
{
	struct inode *i;

	if (f->f_count > 1) {
		f->f_count--;
		return;
	}

	i = f->f_inode;
	if (f->f_op && f->f_op->release && i)
		f->f_op->release(i, f);

	ACQUIRE_LOCK(&file_lock);
	f->f_count--;
	f->f_inode = NULL;
	RELEASE_LOCK(&file_lock);

	if (i)
		iput(i);
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

static struct file *copy_fd(struct file *old_file)
{
	int error;
	struct file *new_file;

	if (!(new_file = get_empty_file()))
		return NULL;
	*new_file = *old_file;
	new_file->f_count = 1;
	if (new_file->f_inode)
		iref(new_file->f_inode);
	if (new_file->f_op && new_file->f_op->open) {
		error = new_file->f_op->open(new_file->f_inode, new_file);
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

	*(p->files) = *(current->files);

	for (i = 0; i < NR_OPEN; i++)
		if ((f = p->files->fd[i]))
			p->files->fd[i] = copy_fd(f);
}

void copy_fs(struct task_struct *p)
{
	*(p->fs) = *(current->fs);

	if (p->fs->pwd)
		iref(p->fs->pwd);
	if (p->fs->root)
		iref(p->fs->root);
}

extern int sys_close(unsigned int fd);
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