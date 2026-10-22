#include <fs/fs.h>

int sys_statx(int dirfd, char *pathname, int flags, unsigned int mask, struct statx *statxbuf)
{
	int err = verify_area(VERIFY_WRITE, statxbuf, sizeof(struct statx));
	if (err)
		return err;

	struct inode *inode;
	err = namei_at(dirfd, pathname, &inode, !(flags & AT_SYMLINK_NOFOLLOW));
	if (err)
		return err;

	struct statx tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.stx_dev_major = MAJOR(inode->i_dev);
	tmp.stx_dev_minor = MINOR(inode->i_dev);
	tmp.stx_rdev_major = MAJOR(inode->i_rdev);
	tmp.stx_rdev_minor = MINOR(inode->i_rdev);
	tmp.stx_ino = inode->i_ino;
	tmp.stx_mode = inode->i_mode | 0777U; // rwx-rwx-rwx
	tmp.stx_nlink = inode->i_nlink;
	tmp.stx_size = inode->i_size;
	tmp.stx_blksize = inode->i_block_size;
	tmp.stx_blocks = inode->i_nr_block;
	tmp.stx_uid = 0;
	tmp.stx_gid = 0;

	tmp.stx_atime.tv_sec = inode->i_atime;
	tmp.stx_mtime.tv_sec = inode->i_mtime;
	tmp.stx_ctime.tv_sec = inode->i_ctime;
	tmp.stx_btime.tv_sec = inode->i_mtime;

	tmp.stx_mask = STATX_BASIC_STATS;
	memcpy_tofs((char *)statxbuf, (char *)&tmp, sizeof(tmp));

	iput(inode);
	return 0;
}

static int statfs64_from_inode(struct inode *inode, struct statfs64 *buf)
{
	struct super_block *sb;

	if (!inode || !inode->i_sb)
		return -ENODEV;

	sb = inode->i_sb;
	if (sb->s_magic == FAT_SUPER_MAGIC) {
		memset(buf, 0, sizeof(*buf));
		buf->f_type = sb->s_magic;
		buf->f_bsize = FAT_CLUSTER_SIZE(sb);
		buf->f_blocks = FAT_SB(sb).total_clusters;
		buf->f_bfree = 0;
		buf->f_bavail = 0;
		buf->f_files = 0;
		buf->f_ffree = 0;
		buf->f_fsid.__val[0] = sb->s_dev;
		buf->f_fsid.__val[1] = 0;
		buf->f_namelen = 255;
		buf->f_frsize = FAT_CLUSTER_SIZE(sb);
		buf->f_flags = 0;
		return 0;
	}
	if (sb->s_magic != MARIO_MAGIC)
		return -ENOSYS;

	memset(buf, 0, sizeof(*buf));
	buf->f_type = sb->s_magic;
	buf->f_bsize = sb->s_block_size;
	buf->f_blocks = MARIO_NR_BLOCKS(sb);
	buf->f_bfree = MARIO_NR_FREE(sb);
	buf->f_bavail = MARIO_NR_FREE(sb);
	buf->f_fsid.__val[0] = sb->s_dev;
	buf->f_fsid.__val[1] = 0;
	buf->f_namelen = MARIO_NAME_LEN - 1;
	buf->f_frsize = sb->s_block_size;
	buf->f_flags = 0;
	return 0;
}

int sys_statfs64(const char *pathname, size_t bufsize, struct statfs64 *buf)
{
	struct statfs64 tmp;
	struct inode *inode;
	int err;

	if (bufsize < sizeof(tmp))
		return -EINVAL;
	err = verify_area(VERIFY_WRITE, buf, sizeof(tmp));
	if (err)
		return err;

	err = namei(pathname, &inode, 1);
	if (err)
		return err;

	err = statfs64_from_inode(inode, &tmp);
	iput(inode);
	if (err)
		return err;

	memcpy_tofs(buf, &tmp, sizeof(tmp));
	return 0;
}

int sys_fstatfs64(unsigned int fd, size_t bufsize, struct statfs64 *buf)
{
	struct statfs64 tmp;
	struct file *file;
	int err;

	if (bufsize < sizeof(tmp))
		return -EINVAL;
	err = verify_area(VERIFY_WRITE, buf, sizeof(tmp));
	if (err)
		return err;

	if (fd >= NR_OPEN || !(file = current->files->fd[fd]) ||
	    !file->f_inode)
		return -EBADF;

	err = statfs64_from_inode(file->f_inode, &tmp);
	if (err)
		return err;

	memcpy_tofs(buf, &tmp, sizeof(tmp));
	return 0;
}


int sys_fchmodat2(int dirfd, const char *pathname, mode_t mode, int flags)
{
	struct inode *inode;
	int err = namei_at(dirfd, pathname, &inode, !(flags & AT_SYMLINK_NOFOLLOW));
	if (err)
		return err;

	iput(inode);
	return 0;
}

int sys_fchmodat(int dirfd, const char *pathname, mode_t mode)
{
	return sys_fchmodat2(dirfd, pathname, mode, 0);
}

int sys_chmod(const char *pathname, mode_t mode)
{
	return sys_fchmodat(AT_FDCWD, pathname, mode);
}

int sys_fchmod(unsigned int fd, mode_t mode)
{
	struct file *f;
	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(f->f_inode))
		return -EBADF;
	return 0;
}

int sys_fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags) {
	struct inode *inode;
	int err = namei_at(dirfd, pathname, &inode, !(flags & AT_SYMLINK_NOFOLLOW));
	if (err)
		return err;

	iput(inode);
	return 0;
}

int sys_lchown(const char *pathname, uid_t owner, gid_t group) {
	return sys_fchownat(AT_FDCWD, pathname, owner, group, AT_SYMLINK_NOFOLLOW);
}

int sys_chown(const char *pathname, uid_t owner, gid_t group) {
	return sys_fchownat(AT_FDCWD, pathname, owner, group, 0);
}

int sys_fchown(unsigned int fd, uid_t owner, gid_t group) {
	struct file *f;
	if (fd >= NR_OPEN || !(f = current->files->fd[fd]) || !(f->f_inode))
		return -EBADF;
	return 0;
}
