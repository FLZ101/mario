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
 * look up one part of a pathname
 * NOTE:
 *   @dir is eaten
 */
static int lookup(struct inode *dir, char *name, int len, struct inode **res)
{
	struct super_block *sb;

	*res = NULL;
	if (!dir)
		return -ENOENT;
	if  (len == 2 && name[0] == '.' && name[1] == '.') {
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

/*
 * dir_namei() returns the inode of the directory of the
 * specified name, and the name within that directory.
 */
static int dir_namei(char *pathname, int *namelen, char **name,
	struct inode *base, struct inode **res_inode)
{
	char c;
	char *thisname;
	int len, error;
	struct inode *inode;

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
	while (1) {
		thisname = pathname;
		for (len = 0; (c = *(pathname++)) && (c != '/'); len++)
			;
		if (!c)
			break;
		error = lookup(base, thisname, len, &inode);
		if (error)
			return error;
		base = inode;
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

int _namei(char *pathname, struct inode *base, struct inode **res_inode)
{
	char *basename;
	int namelen, error;
	struct inode *inode;

	*res_inode = NULL;
	error = dir_namei(pathname, &namelen, &basename, base, &base);
	if (error)
		return error;
	error = lookup(base, basename, namelen, &inode);
	if (error)
		return error;
	*res_inode = inode;
	return 0;
}

int namei(const char *pathname, struct inode **res_inode)
{
	int error;
	char *tmp;

	error = getname(pathname, &tmp);
	if (!error) {
		error = _namei(tmp, NULL, res_inode);
		putname(tmp);
	}
	return error;
}

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
	if (S_ISDIR(inode->i_mode) && (flags & 2)) {
		iput(inode);
		return -EISDIR;
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
