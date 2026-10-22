#include <fs/fs.h>

#include <misc.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

/* Initialized in @blkdev_init() */
dev_t ROOT_DEV = 0;

#define NR_SUPER	32

static struct super_block all[NR_SUPER];

/* To simplify things, only one process can do mount/umount at a time */
static struct semaphore sb_sem;

void sb_init(void)
{
	int i;

	for (i = 0; i < NR_SUPER; i++) {
		all[i].s_dev = 0;
		all[i].s_mountpoint = NULL;
		all[i].s_fsname = NULL;
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

static struct super_block *get_super_by_type(struct file_system_type *fs)
{
	int i;

	for (i = 0; i < NR_SUPER; i++)
		if (all[i].s_fsname == fs->name)
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

	if (dev && (sb = get_super(dev)))
		return sb;

	if (!(fs = get_fs_type(name)))
		return NULL;
	if (!dev && (sb = get_super_by_type(fs)))
		return sb;

	for (sb = all; ; sb++) {
		if (sb >= all + NR_SUPER)
			return NULL;
		if (!sb->s_dev && !sb->s_fsname)
			break;
	}
	sb->s_dev = dev;
	if (!fs->read_super(sb)) {
		sb->s_dev = 0;
		return NULL;
	}
	sb->s_fsname = fs->name;
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
		if (tmp->fs_flags & FS_NODEV)
			continue;
		sb = read_super(ROOT_DEV, tmp->name);
		if (sb) {
			/* NOTE! this inode would be referenced 4 times */
			i = sb->s_mounted;
			iref(i);
			iref(i);
			sb->s_covered = NULL;
			if (!sb->s_mountpoint) {
				sb->s_mountpoint = kmalloc(2);
				if (!sb->s_mountpoint)
					hang("mount_root mountpoint");
				sb->s_mountpoint[0] = '/';
				sb->s_mountpoint[1] = '\0';
			}
			current->fs->pwd = i;
			current->fs->root = i;
		}
	}
	up(&sb_sem);

	if (!sb)
		hang("mount_root fails");
}

extern int fs_may_mount(dev_t dev);
extern int fs_may_umount(struct super_block *sb, struct inode *mount_root);

static int do_mount(dev_t dev, char *dir_name,
		    struct file_system_type *fs_type, char *mountpoint)
{
	struct inode *dir_i;
	struct super_block *sb;
	int error;

	error = namei(dir_name, &dir_i, 0);
	if (error)
		return error;
	if (!S_ISDIR(dir_i->i_mode)) {
		iput(dir_i);
		return -ENOTDIR;
	}

	// Does not allow overlay
	if (dir_i->i_count != 1 || dir_i->i_mount) {
		iput(dir_i);
		return -EBUSY;
	}

	if (dev && !fs_may_mount(dev)) {
		iput(dir_i);
		return -EBUSY;
	}
	sb = read_super(dev, fs_type->name);
	if (!sb) {
		iput(dir_i);
		return -EINVAL;
	}

	// Already mounted
	if (sb->s_covered) {
		iput(dir_i);
		return -EBUSY;
	}

	sb->s_mountpoint = kmalloc(strlen(mountpoint) + 1);
	if (!sb->s_mountpoint) {
		iput(dir_i);
		return -ENOMEM;
	}
	memcpy(sb->s_mountpoint, mountpoint, strlen(mountpoint) + 1);
	sb->s_covered = dir_i;
	dir_i->i_mount = sb->s_mounted;
	return 0;
}

int sys_mount(char *dev_name, char *dir_name, char *type,
	unsigned long mountflags, void *data)
{
	int error;
	char *tmp;
	char *dir_copy;
	dev_t dev;
	struct inode *inode;
	struct file_system_type *fs_type;

	if (getname(type, &tmp))
		return -ENODEV;
	fs_type = get_fs_type(tmp);
	putname(tmp);
	if (!fs_type)
		return -ENODEV;
	error = getname(dir_name, &dir_copy);
	if (error)
		return error;
	dev = 0;
	if (!(fs_type->fs_flags & FS_NODEV)) {
		error = namei(dev_name, &inode, 0);
		if (error)
			goto out;
		if (!S_ISBLK(inode->i_mode)) {
			iput(inode);
			error = -ENOTBLK;
			goto out;
		}
		dev = inode->i_rdev;
		iput(inode);
		if (MAJOR(dev) >= MAX_BLKDEV) {
			error = -ENXIO;
			goto out;
		}
	}
	error = do_mount(dev, dir_name, fs_type, dir_copy);
out:
	putname(dir_copy);
	return error;
}

static int do_umount_sb(struct super_block *sb)
{
	int ret;

	if (!sb || !sb->s_covered)
		return -EINVAL;
	if (!fs_may_umount(sb, sb->s_mounted))
		return -EBUSY;
	sb->s_covered->i_mount = NULL;
	iput(sb->s_covered);
	sb->s_covered = NULL;
	iput(sb->s_mounted);
	sb->s_mounted = NULL;
	if (sb->s_mountpoint) {
		kfree(sb->s_mountpoint);
		sb->s_mountpoint = NULL;
	}
	sb->s_fsname = NULL;
	ret = write_super(sb);
	sb->s_dev = 0; // !!!
	return ret;
}

/*
 * Is it right?
 */
int sys_umount(char *name)
{
	struct inode *inode;
	struct super_block *sb;
	int error;

	error = namei(name, &inode, 0);
	if (error)
		return error;
	if (S_ISBLK(inode->i_mode)) {
		dev_t dev = inode->i_rdev;

		iput(inode);
		if (MAJOR(dev) >= MAX_BLKDEV)
			return -ENXIO;
		if (dev == ROOT_DEV)
			return 0;
		sb = get_super(dev);
		if (!sb || !sb->s_covered)
			return -EINVAL;
	} else if (S_ISDIR(inode->i_mode)) {
		// If inode->i_sb is not mounted or inode is not the root
		if (!inode->i_sb->s_covered || inode->i_sb->s_mounted != inode) {
			iput(inode);
			return -EINVAL;
		}
		sb = inode->i_sb;
		iput(inode);
	} else {
		iput(inode);
		return -EINVAL;
	}
	return do_umount_sb(sb);
}

int sys_umount2(char *name, int flags)
{
	return sys_umount(name);
}

int get_mount_super(int index, struct super_block **sb)
{
	if (index < 0 || index >= NR_SUPER || !sb)
		return -EINVAL;
	if (!all[index].s_mounted || !all[index].s_fsname)
		return -ENOENT;
	*sb = &all[index];
	return 0;
}

int get_nr_mounts(void)
{
	int count = 0;
	int i;

	for (i = 0; i < NR_SUPER; i++)
		if (all[i].s_mounted && all[i].s_fsname)
			count++;
	return count;
}
