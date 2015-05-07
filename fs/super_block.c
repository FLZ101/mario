#include <fs/fs.h>

#include <misc.h>

/* Initialized in @blkdev_init() */
dev_t ROOT_DEV = 0;

#define NR_SUPER	32

static struct super_block all[NR_SUPER];

/* To simplify things, only one process can do mount/umount at a time */
static struct semaphore sb_sem;

void __tinit sb_init(void)
{
	int i;

	for (i = 0; i < NR_SUPER; i++) {
		all[i].s_dev = 0;
		init_MUTEX(&all[i].s_sem);
	}

	init_MUTEX(&sb_sem);
}

static struct super_block *get_super(dev_t dev)
{
	int i;

	if (!dev)
		return NULL;
	for (i = 0; i < NR_SUPER; i++)
		if (all[i].s_dev == dev)
			return all + i;
	return NULL;
}
/*
static int write_super(struct super_block *sb)
{
	if (!sb_dirty(sb))
		return 0;

	clear_bit(SB_Dirty, &(sb)->s_state);
	if (sb->s_op && sb->s_op->write_super)
		return sb->s_op->write_super(sb);
	
	return 0;
}
*/
static struct super_block *read_super(dev_t dev, char *name, void *data)
{
	struct super_block *sb;
	struct file_system_type *fs;

	if ((sb = get_super(dev)))
		return sb;

	if (!(fs = get_fs_type(name)))
		return NULL;

	for (sb = all; ; sb++) {
		if (sb >= all + NR_SUPER)
			return NULL;
		if (!sb->s_dev)
			break;
	}
	sb->s_dev = dev;
	if (!fs->read_super(sb, data)) {
		sb->s_dev = 0;
		return NULL;
	}
	return sb;
}

void mount_root(void)
{
	struct inode *i;
	struct super_block *sb = NULL;
	struct list_head *pos;
	struct file_system_type *tmp;

	down(&sb_sem);
	list_for_each(pos, &file_systems) {
		tmp = list_entry(pos, struct file_system_type, list);
		sb = read_super(ROOT_DEV, tmp->name, NULL);
		if (sb) {
			/* NOTE! this inode would be referenced 4 times */
			i = sb->s_mounted;
			iref(i);
			iref(i);
			iref(i);
			sb->s_covered = i;
			current->fs->pwd = i;
			current->fs->root = i;
		}
	}
	up(&sb_sem);

	if (!sb)
		early_hang("mount_root fails");
}