#include <fs/fs.h>

/*
 * How long a filename can we get from user space?
 *  -EFAULT if invalid area
 *  0 if ok (ENAMETOOLONG before EFAULT)
 *  >0 EFAULT after xx bytes
 */
static inline int get_max_filename(unsigned long address)
{
	struct vm_area_struct * vma;

	if (get_fs() == KERNEL_DS)
		return 0;
	vma = find_vma(current->mm, address);
	if (!vma || vma->vm_start > address || !(vma->vm_flags & VM_READ))
		return -EFAULT;
	address = vma->vm_end - address;
	if (address > PAGE_SIZE)
		return 0;
	if (vma->vm_next && vma->vm_next->vm_start == vma->vm_end &&
	   (vma->vm_next->vm_flags & VM_READ))
		return 0;
	return address;
}

/*
 * POSIX.1 2.4: an empty pathname is invalid (ENOENT).
 *
 * -ENOENT is returned only if `*filename == '\0'`
 */
int getname(const char *filename, char **result)
{
	int i, error;
	unsigned long page;
	char *tmp, c;

	i = get_max_filename((unsigned long) filename);
	if (i < 0)
		return i;
	error = -EFAULT;
	if (!i) {
		error = -ENAMETOOLONG;
		i = PAGE_SIZE;
	}
	c = get_fs_byte(filename++);
	if (!c)
		return -ENOENT;
	if(!(page = page_alloc()))
		return -ENOMEM;
	*result = tmp = (char *)page;
	while (--i) {
		*(tmp++) = c;
		c = get_fs_byte(filename++);
		if (!c) {
			*tmp = '\0';
			return 0;
		}
	}
	page_free(page);
	return error;
}

void putname(char *name)
{
	page_free((unsigned long)name);
}

/*
 * Look up `name` in `dir`. Note that `dir` is eaten
 */
int lookup(struct inode *dir, char *name, int len, struct inode **res)
{
	struct super_block *sb;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if (len == 2 && name[0] == '.' && name[1] == '.') {
		if (dir == current->fs->root) {
			*res = dir;
			return 0;
		} else if ((sb = dir->i_sb) && (dir == sb->s_mounted)) {
			iput(dir);
			if (!(dir = sb->s_covered))
				return -ENOENT;
			iref(dir);
		}
	}
	if (!dir->i_op || !dir->i_op->lookup) {
		iput(dir);
		return -ENOTDIR;
	}
	/* cases like dir/ */
	if (!len) {
		*res = dir;
		return 0;
	}
	return dir->i_op->lookup(dir, name, len, res);
}

int follow_link(struct inode *dir, struct inode *inode,
	int flag, struct inode **res_inode)
{
	if (!dir || !inode) {
		iput(dir);
		iput(inode);
		*res_inode = NULL;
		return -ENOENT;
	}
	if (!inode->i_op || !inode->i_op->follow_link) {
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	return inode->i_op->follow_link(dir, inode, flag, res_inode);
}

/*
 * Returns:
 *
 *   `res_inode`: inode for the dirname
 *
 *   `namelen`, `name`: the basename
 *
 * Note: base is eaten
 *
 * Examples:
 *
 *   pathname: /a/b/c
 *   base: NULL
 *   =>
 *   res_inode: /a/b
 *   name, namelen: c, 1
 *
 *   pathname: a/b/c
 *   base: /x/y
 *   =>
 *   res_inode: /x/y/a/b
 *   name, namelen: c, 1
 */
int dir_namei(char *pathname, int *namelen, char **name,
	struct inode *base, struct inode **res_inode)
{
	char c;
	char *thisname;
	int len, error;
	struct inode *inode = NULL;

	*res_inode = NULL;
	if (!base) {
		base = current->fs->pwd;
		iref(base);
	}
	if ((c = *pathname) == '/') {
		iput(base);
		base = current->fs->root;
		pathname++;
		iref(base);
	}
	assert(S_ISDIR(base->i_mode));

	while (1) {
		thisname = pathname;
		for (len = 0; (c = *(pathname++)) && (c != '/'); len++)
			;
		if (!c)
			break;

		// POSIX.1-2017: "Symbolic links in any component of path other than the last shall be followed."
		iref(base);
		error = lookup(base, thisname, len, &inode);
		if (error) {
			iput(base);
			return error;
		}
		error = follow_link(base, inode, 0, &base);
		if (error)
			return error;
	}
	if (!base->i_op || !base->i_op->lookup) {
		iput(base);
		return -ENOTDIR;
	}
	*name = thisname;
	*namelen = len;
	*res_inode = base;
	return 0;
}

// Note: `base` is eaten
int _namei(char *pathname, struct inode *base, struct inode **res_inode, int follow)
{
	char *basename;
	int namelen, error;
	struct inode *inode;

	*res_inode = NULL;
	error = dir_namei(pathname, &namelen, &basename, base, &base);
	if (error)
		return error;

	iref(base);
	error = lookup(base, basename, namelen, &inode);
	if (error) {
		iput(base);
		return error;
	}
	if (follow) {
		error = follow_link(base, inode, 0, &inode);
		if (error)
			return error;
	} else {
		iput(base);
	}
	*res_inode = inode;
	return 0;
}

int get_base_inode(int dirfd, int path_empty, struct inode **res_inode)
{
	struct file *base_f;
	struct inode* base_i = NULL;

	*res_inode = NULL;
    if (dirfd != AT_FDCWD) {
        if (dirfd >= NR_OPEN || !(base_f = current->files->fd[dirfd]) ||
            !(base_i = base_f->f_inode) ||
            (!path_empty && !S_ISDIR(base_i->i_mode))) {
            return -EBADF;
        }
        iref(base_i);
		*res_inode = base_i;
    }
	return 0;
}

int namei_at(int dirfd, const char *pathname, struct inode **res_inode, int follow)
{
	int error;
	char *tmp;
	int path_empty = 0;

	error = getname(pathname, &tmp);
	if (error) {
		if (error == -ENOENT) // pathname is ""
			path_empty = 1;
		else
			return error;
	}

	struct inode *base_i = NULL;
	error = get_base_inode(dirfd, path_empty, &base_i);
	if (error)
		return error;

    if (path_empty) {
		*res_inode = base_i;
		return 0;
	}

	error = _namei(tmp, base_i, res_inode, follow);

	if (!path_empty)
		putname(tmp);
	return error;
}

// This always go down a mount point
int namei(const char *pathname, struct inode **res_inode, int follow)
{
	return namei_at(AT_FDCWD, pathname, res_inode, follow);
}

// Note: `base` is eaten
int open_namei(char *pathname, int flags, struct inode **res_inode,
	struct inode *base)
{
	char *basename;
	int namelen, error;
	struct inode *dir, *inode;

	error = dir_namei(pathname, &namelen, &basename, base, &dir);
	if (error)
		return error;
	if (!namelen) {		/* cases like dir/ */
		// can not write a dir
		if (flags & 2) {
			iput(dir);
			return -EISDIR;
		}
		*res_inode = dir;
		return 0;
	}

	down(&dir->i_sem);
	iref(dir);	/* lookup eats the dir */
	error = lookup(dir, basename, namelen, &inode);
	if (!(flags & O_CREAT))
		goto SHE__;
	if (!error) {
		if (flags & O_EXCL) {
			iput(inode);
			error = -EEXIST;
		}
		goto SHE__;
	}
	if (!dir->i_op || !dir->i_op->create) {
		error = -EACCES;
		goto SHE__;
	}
	iref(dir);	/* create eats the dir */
	error = dir->i_op->create(dir, basename, namelen, res_inode);
	up(&dir->i_sem);
	iput(dir);
	return error;
SHE__:
	up(&dir->i_sem);
	if (error) {
		iput(dir);
		return error;
	}

	if (S_ISLNK(inode->i_mode)) {
		if (flags & O_NOFOLLOW) {
			iput(inode);
			iput(dir);
			return -ELOOP;
		}
		error = follow_link(dir, inode, flags, &inode);
		if (error)
			return error;
	} else {
		iput(dir);
	}

	// can not write a dir
	if (S_ISDIR(inode->i_mode) && (flags & 2)) {
		iput(inode);
		return -EISDIR;
	}

	if ((flags & O_DIRECTORY) && !S_ISDIR(inode->i_mode)) {
		iput(inode);
		return -ENOTDIR;
	}

	if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
		flags &= ~O_TRUNC;

	if (flags & O_TRUNC) {
		if (inode->i_op && inode->i_op->truncate) {
			down(&inode->i_sem);
			inode->i_op->truncate(inode, 0);
			up(&inode->i_sem);
		}
		set_bit(I_Dirty, &inode->i_state);
	}
	*res_inode = inode;
	return 0;
}

static int do_rmdir(char *name)
{
	char *basename;
	int namelen, error;
	struct inode *dir;

	error = dir_namei(name, &namelen, &basename, NULL, &dir);
	if (error)
		return error;
	if (!namelen) {		/* !!! */
		iput(dir);
		return -ENOENT;
	}
	if (!dir->i_op || !dir->i_op->rmdir) {
		iput(dir);
		return -EPERM;
	}
	return dir->i_op->rmdir(dir, basename, namelen);
}

int sys_rmdir(char *pathname)
{
	int error;
	char *tmp;

	error = getname(pathname, &tmp);
	if (!error) {
		error = do_rmdir(tmp);
		putname(tmp);
	}
	return error;
}

static int do_mkdir(char *name)
{
	char *basename;
	int namelen, error;
	struct inode *dir;

	error = dir_namei(name, &namelen, &basename, NULL, &dir);
	if (error)
		return error;
	if (!namelen) {		/* !!! */
		iput(dir);
		return -ENOENT;
	}
	if (!dir->i_op || !dir->i_op->mkdir) {
		iput(dir);
		return -EPERM;
	}
	return dir->i_op->mkdir(dir, basename, namelen);
}

int sys_mkdir(char *pathname)
{
	int error;
	char *tmp;

	error = getname(pathname, &tmp);
	if (!error) {
		error = do_mkdir(tmp);
		putname(tmp);
	}
	return error;
}

int do_unlinkat(int dirfd, char *name)
{
	char *basename;
	int namelen, error;
	struct inode *dir, *base_i;

	error = get_base_inode(dirfd, 0, &base_i);
	if (error)
		return error;

	error = dir_namei(name, &namelen, &basename, base_i, &dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -EPERM;
	}
	if (!dir->i_op || !dir->i_op->unlink) {
		iput(dir);
		return -EPERM;
	}
	return dir->i_op->unlink(dir, basename, namelen);
}

int sys_unlinkat(int dirfd, const char *pathname, int flags)
{
	int error;
	char *tmp;

	error = getname(pathname, &tmp);
	if (!error) {
		error = do_unlinkat(dirfd, tmp);
		putname(tmp);
	}
	return error;
}

int sys_unlink(char *pathname)
{
	return sys_unlinkat(AT_FDCWD, pathname, 0);
}

static int do_rename(char *oldname, char *newname)
{
	struct inode *old_dir, *new_dir;
	char *old_base, *new_base;
	int old_len, new_len, error;

	error = dir_namei(oldname, &old_len, &old_base, NULL, &old_dir);
	if (error)
		return error;
	if (!old_len || (old_base[0] == '.' &&
		(old_len == 1 || (old_base[1] == '.' &&
			old_len == 2)))) {
		iput(old_dir);
		return -EPERM;
	}
	error = dir_namei(newname, &new_len, &new_base, NULL, &new_dir);
	if (error) {
		iput(old_dir);
		return error;
	}
	if (!new_len || (new_base[0] == '.' &&
		(new_len == 1 || (new_base[1] == '.' &&
			new_len == 2)))) {
		iput(old_dir);
		iput(new_dir);
		return -EPERM;
	}
	if (new_dir->i_dev != old_dir->i_dev) {
		iput(old_dir);
		iput(new_dir);
		return -EXDEV;
	}
	if (!old_dir->i_op || !old_dir->i_op->rename) {
		iput(old_dir);
		iput(new_dir);
		return -EPERM;
	}
	return old_dir->i_op->rename(old_dir, old_base, old_len,
		new_dir, new_base, new_len);
}

int sys_rename(char *oldname, char *newname)
{
	int error;
	char *from, *to;

	error = getname(oldname, &from);
	if (!error) {
		error = getname(newname, &to);
		if (!error) {
			error = do_rename(from, to);
			putname(to);
		}
		putname(from);
	}
	return error;
}

static int do_mknod(char *filename, int mode, dev_t dev)
{
	char *basename;
	int namelen, error;
	struct inode *dir;

	error = dir_namei(filename, &namelen, &basename, NULL, &dir);
	if (error)
		return error;
	if (!namelen) {
		iput(dir);
		return -ENOENT;
	}
	if (!dir->i_op || !dir->i_op->mknod) {
		iput(dir);
		return -EPERM;
	}
	return dir->i_op->mknod(dir, basename, namelen, mode, dev);
}

int sys_mknod(char *filename, int mode, dev_t dev)
{
	int error;
	char *tmp;

	if (S_ISDIR(mode))
		return -EPERM;
	if (mode < MODE_REG || mode > MODE_CHR)
		return -EINVAL;
	error = getname(filename, &tmp);
	if (!error) {
		error = do_mknod(tmp, mode, dev);
		putname(tmp);
	}
	return error;
}

int sys_readlinkat(unsigned int fd, char *pathname, char *buf, size_t count)
{
	if (!count)
		return 0;
	int error = verify_area(VERIFY_WRITE, buf, count);
	if (error)
		return error;

	struct inode *inode;
	error = namei_at(fd, pathname, &inode, 0);
	if (error)
		return error;
	if (!inode->i_op || !inode->i_op->readlink || !S_ISLNK(inode->i_mode)) {
		iput(inode);
		return -EINVAL;
	}
	return inode->i_op->readlink(inode, buf, count);
}

int sys_readlink(char *pathname, char *buf, size_t count)
{
	return sys_readlinkat(AT_FDCWD, pathname, buf, count);
}
