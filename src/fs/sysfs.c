#include <fs/fs.h>
#include <fs/sysfs.h>
#include <fs/dirent.h>

#include <lib/string.h>

#define SYSFS_INO_ROOT	1UL
#define SYSFS_INO_KERNEL	2UL
#define SYSFS_INO_DEBUG	3UL

static struct file_operations sysfs_dir_fops;

static struct super_block *sysfs_sb;

static int sysfs_name_is(char *name, int len, char *want)
{
	int n = strlen(want);

	return len == n && !memcmp(name, want, n);
}

static struct inode *sysfs_alloc_inode(unsigned long ino,
				       struct inode_operations *iop,
				       struct file_operations *fop)
{
	struct inode *inode = get_empty_inode();

	if (!inode)
		return NULL;

	inode->i_dev = sysfs_sb ? sysfs_sb->s_dev : 0;
	inode->i_ino = ino;
	inode->i_sb = sysfs_sb;
	inode->i_rdev = 0;
	inode->i_mount = NULL;
	inode->i_nr_block = 0;
	inode->i_state = 0;
	inode->i_mode = S_IFDIR | 0555;
	inode->i_nlink = 2;
	inode->i_size = 0;
	inode->i_block_size = 4096;
	inode->i_op = iop;
	inode->i_fop = fop;
	inode->i_atime = inode->i_mtime = inode->i_ctime = 0;
	return inode;
}

static struct inode *sysfs_make_dir(unsigned long ino)
{
	return sysfs_alloc_inode(ino, &sysfs_dir_iops, &sysfs_dir_fops);
}

static struct inode *sysfs_mounted_child(struct super_block *parent,
					 unsigned long ino)
{
	int index;

	for (index = 0; ; index++) {
		struct super_block *child;

		if (get_mount_super(index, &child))
			break;
		if (child->s_covered && child->s_covered->i_sb == parent &&
		    child->s_covered->i_ino == ino && child->s_mounted) {
			iref(child->s_mounted);
			return child->s_mounted;
		}
	}

	return NULL;
}

static int sysfs_lookup_child(unsigned long dir_ino, char *name, int len,
			      struct inode **res)
{
	*res = NULL;

	if (dir_ino == SYSFS_INO_ROOT) {
		if (sysfs_name_is(name, len, "kernel"))
			*res = sysfs_make_dir(SYSFS_INO_KERNEL);
		return *res ? 0 : -ENOENT;
	}

	if (dir_ino == SYSFS_INO_KERNEL) {
		if (sysfs_name_is(name, len, "debug")) {
			*res = sysfs_mounted_child(sysfs_sb, SYSFS_INO_DEBUG);
			if (!*res)
				*res = sysfs_make_dir(SYSFS_INO_DEBUG);
		}
		return *res ? 0 : -ENOENT;
	}

	return -ENOTDIR;
}

static int sysfs_parent(unsigned long dir_ino, struct inode **res)
{
	*res = NULL;

	if (dir_ino == SYSFS_INO_ROOT) {
		if (!sysfs_sb || !sysfs_sb->s_covered)
			return -ENOENT;
		*res = sysfs_sb->s_covered;
		iref(*res);
		return 0;
	}

	if (dir_ino == SYSFS_INO_KERNEL) {
		*res = sysfs_make_dir(SYSFS_INO_ROOT);
		return *res ? 0 : -ENOMEM;
	}

	if (dir_ino == SYSFS_INO_DEBUG) {
		*res = sysfs_make_dir(SYSFS_INO_KERNEL);
		return *res ? 0 : -ENOMEM;
	}

	return -ENOENT;
}

static int sysfs_lookup(struct inode *dir, char *name, int len,
			struct inode **res)
{
	int error;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if (!S_ISDIR(dir->i_mode)) {
		iput(dir);
		return -ENOENT;
	}

	if (len == 1 && name[0] == '.') {
		*res = dir;
		return 0;
	}

	if (len == 2 && name[0] == '.' && name[1] == '.') {
		error = sysfs_parent(dir->i_ino, res);
		iput(dir);
		return error;
	}

	error = sysfs_lookup_child(dir->i_ino, name, len, res);
	iput(dir);
	return error;
}

struct inode_operations sysfs_dir_iops = {
	.lookup = sysfs_lookup,
};

static int sysfs_get_entry(unsigned long dir_ino, int index, char *name,
			   size_t size, ino_t *ino, unsigned char *type)
{
	if (dir_ino == SYSFS_INO_ROOT) {
		if (index != 0)
			return -ENOENT;
		sprintkn(name, size, "%s", "kernel");
		*ino = SYSFS_INO_KERNEL;
		*type = DT_DIR;
		return 0;
	}

	if (dir_ino == SYSFS_INO_KERNEL) {
		if (index != 0)
			return -ENOENT;
		sprintkn(name, size, "%s", "debug");
		*ino = SYSFS_INO_DEBUG;
		*type = DT_DIR;
		return 0;
	}

	return -ENOTDIR;
}

static int sysfs_readdir(struct inode *dir, struct file *file, void *dirent,
			 filldir_t fn)
{
	char name[32];
	ino_t ino;
	unsigned char type;
	int index = file->f_pos;

	if (sysfs_get_entry(dir->i_ino, index, name, sizeof(name),
			    &ino, &type))
		return 0;

	if (fn(dirent, name, strlen(name), index, ino, type))
		return 0;

	file->f_pos = index + 1;
	return 0;
}

static struct file_operations sysfs_dir_fops = {
	.readdir = sysfs_readdir,
};

struct inode *sysfs_get_root(void)
{
	if (!sysfs_sb || !sysfs_sb->s_mounted)
		return NULL;
	iref(sysfs_sb->s_mounted);
	return sysfs_sb->s_mounted;
}

static struct super_operations sysfs_sops;

static struct super_block *sysfs_read_super(struct super_block *sb)
{
	sysfs_sb = sb;
	sb->s_magic = SYSFS_SUPER_MAGIC;
	sb->s_block_size = 4096;
	sb->s_op = &sysfs_sops;
	sb->s_mounted = sysfs_make_dir(SYSFS_INO_ROOT);
	if (!sb->s_mounted)
		return NULL;
	sb->s_mounted->i_sb = sb;
	return sb;
}

struct file_system_type sysfs = {
	.read_super = sysfs_read_super,
	.list = LIST_HEAD_INIT(sysfs.list),
	.name = "sysfs",
	.fs_flags = FS_NODEV,
};
