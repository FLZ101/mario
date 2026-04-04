#include <fs/fs.h>
#include <fs/dirent.h>

#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
#define ROUND_UP(x) (((x)+sizeof(long)-1) & ~(sizeof(long)-1))

struct getdents_callback {
	void *prev; // previous dirent
	void *next; // next dirent to write
	int count;
	int error;
};

static int filldir(void *__buf, char *name, int namelen,
	off_t offset, ino_t ino, unsigned char type)
{
	struct mario_dirent *dirent;
	struct getdents_callback *buf = (struct getdents_callback *)__buf;
	int reclen = ROUND_UP(NAME_OFFSET(dirent) + namelen + 1);

	buf->error = -EINVAL;
	if (reclen > buf->count)
		return -EINVAL;
	dirent = (struct mario_dirent *) buf->prev;
	if (dirent)
		put_fs_long(offset, &dirent->d_off);
	dirent = buf->next;
	buf->prev = dirent;
	put_fs_long(ino, &dirent->d_ino);
	put_fs_word(reclen, &dirent->d_reclen);
	put_fs_byte(type, &dirent->d_type);
	memcpy_tofs(dirent->d_name, name, namelen);
	put_fs_byte(0, dirent->d_name + namelen);
	buf->next = (char *)dirent + reclen;
	buf->count -= reclen;
	return 0;
}

static int filldir64(void *__buf, char *name, int namelen,
	off_t offset, ino_t ino, unsigned char type)
{
	struct mario_dirent64 *dirent;
	struct getdents_callback *buf = (struct getdents_callback *)__buf;
	int reclen = ROUND_UP(NAME_OFFSET(dirent) + namelen + 1);

	buf->error = -EINVAL;
	if (reclen > buf->count)
		return -EINVAL;
	dirent = (struct mario_dirent64 *) buf->prev;
	if (dirent)
		put_fs_long_long(offset, &dirent->d_off);
	dirent = buf->next;
	buf->prev = dirent;
	put_fs_long_long(ino, &dirent->d_ino);
	put_fs_word(reclen, &dirent->d_reclen);
	put_fs_byte(type, &dirent->d_type);
	memcpy_tofs(dirent->d_name, name, namelen);
	put_fs_byte(0, dirent->d_name + namelen);
	buf->next = (char *)dirent + reclen;
	buf->count -= reclen;
	return 0;
}

static int kernel_filldir64(void *__buf, char *name, int namelen,
	off_t offset, ino_t ino, unsigned char type)
{
	struct mario_dirent64 *dirent;
	struct getdents_callback *buf = (struct getdents_callback *)__buf;
	int reclen = ROUND_UP(NAME_OFFSET(dirent) + namelen + 1);

	buf->error = -EINVAL;
	if (reclen > buf->count)
		return -EINVAL;
	dirent = (struct mario_dirent64 *) buf->prev;
	if (dirent)
		dirent->d_off = offset;

	dirent = buf->next;
	buf->prev = dirent;

	dirent->d_ino = ino;
	dirent->d_reclen = reclen;
	dirent->d_type = type;
	memcpy(dirent->d_name, name, namelen);
	dirent->d_name[namelen] = '\0';

	buf->next = (char *)dirent + reclen;
	buf->count -= reclen;
	return 0;
}

static int do_getdents(struct file *file, void *dirent, unsigned int count, filldir_t fn)
{
	struct getdents_callback buf;
	buf.next = dirent;
	buf.prev = NULL;
	buf.count = count;
	buf.error = 0;

	int error = file->f_op->readdir(file->f_inode, file, &buf, fn);
	if (error < 0)
		return error;
	if (fn == filldir) {
		struct mario_dirent *lastdirent = buf.prev;
		if (!lastdirent)
			return buf.error;
		put_fs_long(file->f_pos, &lastdirent->d_off);
	} else if (fn == filldir64) {
		struct mario_dirent64 *lastdirent = buf.prev;
		if (!lastdirent)
			return buf.error;
		put_fs_long_long(file->f_pos, &lastdirent->d_off);
	} else {
		struct mario_dirent64 *lastdirent = buf.prev;
		if (!lastdirent)
			return buf.error;
		lastdirent->d_off = file->f_pos;
	}
	return count - buf.count;
}

static int _do_getdents(unsigned int fd, void *dirent, unsigned int count, filldir_t fn)
{
	struct file *file;
	int error;

	if (fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (!file->f_op || !file->f_op->readdir)
		return -ENOTDIR;
	error = verify_area(VERIFY_WRITE, dirent, count);
	if (error)
		return error;
	return do_getdents(file, dirent, count, fn);
}

int sys_getdents(unsigned int fd, void *dirent, unsigned int count)
{
	return _do_getdents(fd, dirent, count, filldir);
}

int sys_getdents64(unsigned int fd, void *dirent, unsigned int count)
{
	return _do_getdents(fd, dirent, count, filldir64);
}

extern int lookup(struct inode *dir, char *name, int len, struct inode **res);

// NOTE: inode is eaten
static struct inode *get_parent_dir_inode(struct inode *inode)
{
	assert(S_ISDIR(inode->i_mode));

	struct inode *parent = NULL;

	int err = lookup(inode, "..", 2, &parent);
	assert(!err && parent && "Fail to get parent directory's inode");

	return parent;
}

// Search a dirent by ino and copy its name to end of the buffer [begin, *end)
static int find_dirent(struct inode *dir, ino_t ino, char *begin, char **end)
{
	int err = 0;

	// a dummy file
	struct file *f = get_empty_file();
	if (!f) {
		err = -ENOMEM;
		goto tail_1;
	}
	f->f_inode = dir;
	f->f_pos = 0;
	f->f_mode = S_IFDIR;
	f->f_op = dir->i_fop;
	f->f_flags = O_RDONLY;

	void *buf = (void *) zero_page_alloc();
	if (!buf) {
		err = -ENOMEM;
		goto tail_2;
	}

	while (1) {
		int n = do_getdents(f, buf, PAGE_SIZE, kernel_filldir64);
		if (n < 0) {
			err = n;
			goto tail_3;
		}
		// End of dir
		if (n == 0) {
			err = -ENOENT;
			break;
		}

        int off = 0;
        while (off < n) {
            struct mario_dirent64 *ent = (struct mario_dirent64 *) (buf + off);
			if (ent->d_ino == ino) {
				int namelen = strlen(ent->d_name);
				if (begin + namelen + 1 > *end) {
					err = -ERANGE;
					goto tail_3;
				}
				*end -= namelen + 1;
				put_fs_byte('/', *end);
				memcpy_tofs(*end + 1, ent->d_name, namelen);
				goto tail_3;
			}
            off += ent->d_reclen;
        }
	}

tail_3:
	page_free((unsigned long) buf);
tail_2:
	f->f_inode = NULL;
	f->f_op = NULL;
	put_file(f);
tail_1:
	return err;
}

// Return the number of bytes filled
int sys_getcwd(char *buf, unsigned long size)
{
	if (!buf || size < 2)
		return -EINVAL;

	int err = verify_area(VERIFY_WRITE, buf, size);
	if (err)
		return err;

	struct inode *inode = current->fs->pwd;
	iref(inode);

	struct inode *parent = NULL;

	char *begin = buf, *end = buf + size;
	put_fs_byte(0, --end);
	while (1) {
		if (inode == current->fs->root)
			break;

		// Cross filesystem boundary
		if ((inode == inode->i_sb->s_mounted) && inode->i_sb->s_covered) {
			struct inode *tmp = inode;
			inode = inode->i_sb->s_covered;
			iref(inode);
			iput(tmp);
		}

		ino_t ino = inode->i_ino;
		parent = get_parent_dir_inode(inode); // inode is eaten
		if (inode == parent)
			break;

		err = find_dirent(parent, ino, begin, &end);
		if (err < 0) {
			iput(parent);
			return err;
		}

		inode = parent;
	}

	err = 0;
	if (begin + 1 > end) {
		err = -ERANGE;
		goto tail;
	}
	// Outside the chroot jail
	if (inode != current->fs->root) {
		err = -ENOENT;
		goto tail;
	}

	if (get_fs_byte(end) == '\0')
		put_fs_byte('/', --end);
	memcpy_fs(buf, end, buf + size - end);
	err = buf + size - end;

tail:
	iput(inode);
	return err;
}
