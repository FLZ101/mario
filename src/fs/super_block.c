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

static int write_super(struct super_block *sb)
{
	if (!sb_dirty(sb))
		return 0;

	clear_bit(SB_Dirty, &(sb)->s_state);
	if (sb->s_op && sb->s_op->write_super)
		return sb->s_op->write_super(sb);

	return 0;
}

static struct super_block *read_super(dev_t dev, char *name)
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
	if (!fs->read_super(sb)) {
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
		sb = read_super(ROOT_DEV, tmp->name);
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
		hang("mount_root fails");
}

extern int fs_may_mount(dev_t dev);
extern int fs_may_umount(dev_t dev, struct inode *mount_root);

static int do_mount(dev_t dev, char *dir_name, char *type)
{
	struct inode *dir_i;
	struct super_block *sb;
	int error;

	error = namei(dir_name, &dir_i);
	if (error)
		return error;
	if (dir_i->i_count != 1 || dir_i->i_mount) {
		iput(dir_i);
		return -EBUSY;
	}
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -ENOTDIR;
	}
	if (!fs_may_mount(dev)) {
		iput(dir_i);
		return -EBUSY;
	}
	sb = read_super(dev, type);
	if (!sb) {
		iput(dir_i);
		return -EINVAL;
	}
	if (sb->s_covered) {
		iput(dir_i);
		return -EBUSY;
	}
	sb->s_covered = dir_i;
	dir_i->i_mount = sb->s_mounted;
	return 0;
}

int sys_mount(char *dev_name, char *dir_name, char *type)
{
	int error;
	char *tmp;
	dev_t dev;
	struct inode *inode;
	struct file_system_type *fs_type;

	if (getname(type, &tmp))
		return -ENODEV;
	fs_type = get_fs_type(tmp);
	putname(tmp);
	if (!fs_type)
		return -ENODEV;
	error = namei(dev_name, &inode);
	if (error)
		return error;
	if (!S_ISBLK(inode->i_mode)) {
		iput(inode);
		return -ENOTBLK;
	}
	dev = inode->i_rdev;
	iput(inode);
	if (MAJOR(dev) >= MAX_BLKDEV)
		return -ENXIO;
	return do_mount(dev, dir_name, fs_type->name);
}

static int do_umount(dev_t dev)
{
	struct super_block *sb;

	/*
	 * I don't want to umount ROOT_DEV
	 */
	if (dev == ROOT_DEV)
		return 0;
	if (!(sb = get_super(dev)) || !(sb->s_covered))
		return -ENOENT;
	if (!fs_may_umount(dev, sb->s_mounted))
		return -EBUSY;
	sb->s_covered->i_mount = NULL;
	iput(sb->s_covered);
	sb->s_covered = NULL;
	iput(sb->s_mounted);
	sb->s_mounted = NULL;
	return write_super(sb);
}

/*
 * Is it right?
 */
int sys_umount(char *name)
{
	struct inode *inode;
	dev_t dev;
	int error;

	error = namei(name, &inode);
	if (error)
		return error;
	if (S_ISBLK(inode->i_mode)) {
		dev = inode->i_rdev;
	} else if (S_ISDIR(inode->i_mode)) {
		// If inode->i_sb is not mounted or inode is not the root
		if (!inode->i_sb->s_covered || inode->i_sb->s_mounted != inode)
			return -EINVAL;
		dev = inode->i_dev;
	} else {
		return -EINVAL;
	}
	iput(inode);
	if (MAJOR(dev) >= MAX_BLKDEV)
		return -ENXIO;
	return do_umount(dev);
}