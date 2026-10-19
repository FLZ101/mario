#include <fs/fs.h>
#include <fs/debugfs.h>
#include <fs/dirent.h>

#include <syscall_trace.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

#define DEBUGFS_INO_ROOT	1UL
#define DEBUGFS_INO_KERNEL	2UL
#define DEBUGFS_INO_DEBUG	3UL
#define DEBUGFS_INO_SYSCALLS	4UL
#define DEBUGFS_INO_TRACE_PID	5UL

#define DEBUGFS_SYSCALLS_BUF	8192

static struct file_operations debugfs_dir_fops;

static int debugfs_name_is(char *name, int len, char *want)
{
	int n = strlen(want);

	return len == n && !memcmp(name, want, n);
}

static mode_t debugfs_inode_mode(unsigned long ino)
{
	if (ino == DEBUGFS_INO_SYSCALLS || ino == DEBUGFS_INO_TRACE_PID)
		return S_IFREG | 0644;
	return S_IFDIR | 0555;
}

static struct inode *debugfs_alloc_inode(unsigned long ino, int size,
					 struct inode_operations *iop,
					 struct file_operations *fop)
{
	struct inode *inode = get_empty_inode();

	if (!inode)
		return NULL;

	inode->i_dev = 0;
	inode->i_ino = ino;
	inode->i_sb = NULL;
	inode->i_rdev = 0;
	inode->i_mount = NULL;
	inode->i_nr_block = 0;
	inode->i_state = 0;
	inode->i_mode = debugfs_inode_mode(ino);
	inode->i_nlink = S_ISDIR(inode->i_mode) ? 2 : 1;
	inode->i_size = size;
	inode->i_block_size = 4096;
	inode->i_op = iop;
	inode->i_fop = fop;
	inode->i_atime = inode->i_mtime = inode->i_ctime = 0;
	return inode;
}

static int debugfs_sput_u64(char *buf, uint64_t value)
{
	char tmp[20];
	int i = sizeof(tmp);
	int len = 0;

	if (!value)
		return sprintk(buf, "0");

	while (value) {
		tmp[--i] = "0123456789"[value % 10];
		value /= 10;
	}
	while (i < sizeof(tmp))
		buf[len++] = tmp[i++];
	buf[len] = '\0';
	return len;
}

static int debugfs_syscalls_content(char *buf, size_t size)
{
	char line[192];
	int n = 0;
	unsigned int nr;

	if (!size)
		return 0;

	n += sprintk(buf, "syscall name calls cycles avg_cycles max_cycles\n");

	for (nr = 0; nr < NR_SYSCALLS; nr++) {
		struct syscall_trace_stat *stat = &syscall_trace[nr];
		uint64_t avg;
		int len = 0;

		if (!stat->calls)
			continue;

		avg = stat->cycles / stat->calls;
		len += sprintk(line + len, "%u ", nr);
		if (syscall_trace_name(nr))
			len += sprintk(line + len, "%s ",
				       syscall_trace_name(nr));
		else
			len += sprintk(line + len, "- ");
		len += debugfs_sput_u64(line + len, stat->calls);
		len += sprintk(line + len, " ");
		len += debugfs_sput_u64(line + len, stat->cycles);
		len += sprintk(line + len, " ");
		len += debugfs_sput_u64(line + len, avg);
		len += sprintk(line + len, " ");
		len += debugfs_sput_u64(line + len, stat->max_cycles);
		len += sprintk(line + len, "\n");

		if ((size_t)n + (size_t)len >= size)
			break;
		memcpy(buf + n, line, len);
		n += len;
	}

	return n;
}

static int debugfs_trace_pid_content(char *buf, size_t size)
{
	if (size < 16)
		return -ENOSPC;
	return sprintk(buf, "%d\n", syscall_trace_get_pid());
}

static int debugfs_parse_pid(char *buf, int len, pid_t *pid)
{
	int n = 0;
	int i = 0;
	int digits = 0;

	while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
			   buf[i] == '\r' || buf[i] == '\n'))
		i++;
	while (i < len && buf[i] >= '0' && buf[i] <= '9') {
		n = n * 10 + buf[i] - '0';
		if (n > 32768)
			return -EINVAL;
		digits++;
		i++;
	}
	while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
			   buf[i] == '\r' || buf[i] == '\n'))
		i++;
	if (!digits || i != len)
		return -EINVAL;

	*pid = n;
	return 0;
}

static int debugfs_file_read(struct inode *inode, struct file *file,
			     char *buf, int count)
{
	char *content;
	int size;

	if (inode->i_ino == DEBUGFS_INO_TRACE_PID) {
		char tmp[16];

		size = debugfs_trace_pid_content(tmp, sizeof(tmp));
		if (size < 0)
			return size;
		if (file->f_pos >= size)
			return 0;
		if (count > size - file->f_pos)
			count = size - file->f_pos;
		memcpy_tofs(buf, tmp + file->f_pos, count);
		file->f_pos += count;
		return count;
	}

	if (inode->i_ino != DEBUGFS_INO_SYSCALLS)
		return -EINVAL;

	content = kmalloc(DEBUGFS_SYSCALLS_BUF);
	if (!content)
		return -ENOMEM;

	size = debugfs_syscalls_content(content, DEBUGFS_SYSCALLS_BUF);
	if (file->f_pos >= size) {
		kfree(content);
		return 0;
	}
	if (count > size - file->f_pos)
		count = size - file->f_pos;
	memcpy_tofs(buf, content + file->f_pos, count);
	file->f_pos += count;
	kfree(content);
	return count;
}

static int debugfs_file_write(struct inode *inode, struct file *file,
			      char *buf, int count)
{
	char tmp[16];
	pid_t pid;
	int error;

	(void)file;

	if (inode->i_ino == DEBUGFS_INO_SYSCALLS) {
		syscall_trace_reset();
		return count;
	}
	if (inode->i_ino == DEBUGFS_INO_TRACE_PID) {
		if (count <= 0)
			return -EINVAL;
		if (count > sizeof(tmp) - 1)
			count = sizeof(tmp) - 1;
		memcpy_fromfs(tmp, buf, count);
		tmp[count] = '\0';
		error = debugfs_parse_pid(tmp, count, &pid);
		if (error)
			return error;
		syscall_trace_set_pid(pid);
		return count;
	}

	return -EINVAL;
}

static struct file_operations debugfs_file_fops = {
	.read = debugfs_file_read,
	.write = debugfs_file_write,
};

static struct inode *debugfs_make_dir(unsigned long ino)
{
	return debugfs_alloc_inode(ino, 0, &debugfs_dir_iops,
				   &debugfs_dir_fops);
}

static struct inode *debugfs_make_file(unsigned long ino)
{
	int size = 0;

	if (ino == DEBUGFS_INO_SYSCALLS)
		size = DEBUGFS_SYSCALLS_BUF;
	else if (ino == DEBUGFS_INO_TRACE_PID)
		size = 16;
	return debugfs_alloc_inode(ino, size, NULL, &debugfs_file_fops);
}

static int debugfs_lookup_child(unsigned long dir_ino, char *name, int len,
				struct inode **res)
{
	*res = NULL;

	if (dir_ino == DEBUGFS_INO_ROOT) {
		if (debugfs_name_is(name, len, "kernel"))
			*res = debugfs_make_dir(DEBUGFS_INO_KERNEL);
		return *res ? 0 : -ENOENT;
	}

	if (dir_ino == DEBUGFS_INO_KERNEL) {
		if (debugfs_name_is(name, len, "debug"))
			*res = debugfs_make_dir(DEBUGFS_INO_DEBUG);
		return *res ? 0 : -ENOENT;
	}

	if (dir_ino == DEBUGFS_INO_DEBUG) {
		if (debugfs_name_is(name, len, "syscalls"))
			*res = debugfs_make_file(DEBUGFS_INO_SYSCALLS);
		else if (debugfs_name_is(name, len, "trace_pid"))
			*res = debugfs_make_file(DEBUGFS_INO_TRACE_PID);
		return *res ? 0 : -ENOENT;
	}

	return -ENOTDIR;
}

static int debugfs_parent(unsigned long dir_ino, struct inode **res)
{
	*res = NULL;

	if (dir_ino == DEBUGFS_INO_ROOT) {
		*res = current->fs->root;
		iref(*res);
		return 0;
	}
	if (dir_ino == DEBUGFS_INO_KERNEL) {
		*res = debugfs_make_dir(DEBUGFS_INO_ROOT);
		return *res ? 0 : -ENOMEM;
	}
	if (dir_ino == DEBUGFS_INO_DEBUG) {
		*res = debugfs_make_dir(DEBUGFS_INO_KERNEL);
		return *res ? 0 : -ENOMEM;
	}
	if (dir_ino == DEBUGFS_INO_SYSCALLS) {
		*res = debugfs_make_dir(DEBUGFS_INO_DEBUG);
		return *res ? 0 : -ENOMEM;
	}
	if (dir_ino == DEBUGFS_INO_TRACE_PID) {
		*res = debugfs_make_dir(DEBUGFS_INO_DEBUG);
		return *res ? 0 : -ENOMEM;
	}

	return -ENOENT;
}

static int debugfs_lookup(struct inode *dir, char *name, int len,
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
		error = debugfs_parent(dir->i_ino, res);
		iput(dir);
		return error;
	}

	error = debugfs_lookup_child(dir->i_ino, name, len, res);
	iput(dir);
	return error;
}

struct inode_operations debugfs_dir_iops = {
	.lookup = debugfs_lookup,
};

static int debugfs_get_entry(unsigned long dir_ino, int index, char *name,
			     size_t size, ino_t *ino, unsigned char *type)
{
	(void)size;

	if (dir_ino == DEBUGFS_INO_ROOT) {
		if (index != 0)
			return -ENOENT;
		sprintk(name, "%s", "kernel");
		*ino = DEBUGFS_INO_KERNEL;
		*type = DT_DIR;
		return 0;
	}

	if (dir_ino == DEBUGFS_INO_KERNEL) {
		if (index != 0)
			return -ENOENT;
		sprintk(name, "%s", "debug");
		*ino = DEBUGFS_INO_DEBUG;
		*type = DT_DIR;
		return 0;
	}

	if (dir_ino == DEBUGFS_INO_DEBUG) {
		switch (index) {
		case 0:
			sprintk(name, "%s", "syscalls");
			*ino = DEBUGFS_INO_SYSCALLS;
			*type = DT_REG;
			return 0;
		case 1:
			sprintk(name, "%s", "trace_pid");
			*ino = DEBUGFS_INO_TRACE_PID;
			*type = DT_REG;
			return 0;
		default:
			return -ENOENT;
		}
	}

	return -ENOTDIR;
}

static int debugfs_readdir(struct inode *dir, struct file *file, void *dirent,
			   filldir_t fn)
{
	char name[32];
	ino_t ino;
	unsigned char type;
	int index = file->f_pos;

	if (debugfs_get_entry(dir->i_ino, index, name, sizeof(name),
			      &ino, &type))
		return 0;

	if (fn(dirent, name, strlen(name), index, ino, type))
		return 0;

	file->f_pos = index + 1;
	return 0;
}

static struct file_operations debugfs_dir_fops = {
	.readdir = debugfs_readdir,
};

struct inode *debugfs_get_root(void)
{
	return debugfs_make_dir(DEBUGFS_INO_ROOT);
}
