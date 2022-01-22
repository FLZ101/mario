#include <fs/fs.h>

#define NAME_OFFSET(de) ((int) ((de)->d_name - (char *) (de)))
#define ROUND_UP(x) (((x)+sizeof(long)-1) & ~(sizeof(long)-1))

struct mario_dirent {
	unsigned long d_ino;
	unsigned long d_off;
	unsigned short d_reclen;
	char d_name[1];
};

struct getdents_callback {
	struct mario_dirent *prev;
	struct mario_dirent *next;
	int count;
	int error;
};

static int filldir(void *__buf, char *name, int namelen, off_t offset, ino_t ino)
{
	struct mario_dirent *dirent;
	struct getdents_callback *buf = (struct getdents_callback *)__buf;
	int reclen = ROUND_UP(NAME_OFFSET(dirent) + namelen + 1);

	buf->error = -EINVAL;
	if (reclen > buf->count)
		return -EINVAL;
	dirent = buf->prev;
	if (dirent)
		put_fs_long(offset, &dirent->d_off);
	dirent = buf->next;
	buf->prev = dirent;
	put_fs_long(ino, &dirent->d_ino);
	put_fs_word(reclen, &dirent->d_reclen);
	memcpy_tofs(dirent->d_name, name, namelen);
	put_fs_byte(0, dirent->d_name + namelen);
	buf->next = (struct mario_dirent *)((char *)dirent + reclen);;
	buf->count -= reclen;
	return 0;
}

int sys_getdents(unsigned int fd, void *dirent, unsigned int count)
{
	struct file *file;
	struct mario_dirent *lastdirent;
	struct getdents_callback buf;
	int error;

	if (fd >= NR_OPEN || !(file = current->files->fd[fd]))
		return -EBADF;
	if (!file->f_op || !file->f_op->readdir)
		return -ENOTDIR;
	error = verify_area(VERIFY_WRITE, dirent, count);
	if (error)
		return error;
	buf.next = (struct mario_dirent *)dirent;
	buf.prev = NULL;
	buf.count = count;
	buf.error = 0;
	error = file->f_op->readdir(file->f_inode, file, &buf, filldir);
	if (error < 0)
		return error;
	lastdirent = buf.prev;
	if (!lastdirent)
		return buf.error;
	put_fs_long(file->f_pos, &lastdirent->d_off);
	return count - buf.count;
}