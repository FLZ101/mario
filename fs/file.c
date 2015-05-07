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
 * I need to feel safe
 * Increase reference count of a file by 1
 * This function should be called when reference count of that file is not 0
 */
void fref(struct file *f)
{
	ACQUIRE_LOCK(&file_lock);
	f->f_count++;
	RELEASE_LOCK(&file_lock);
}
