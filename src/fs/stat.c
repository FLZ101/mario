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
	tmp.stx_mask = STATX_BASIC_STATS;
	memcpy_tofs((char *)statxbuf, (char *)&tmp, sizeof(tmp));

	iput(inode);
	return 0;
}
