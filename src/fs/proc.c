#include <fs/fs.h>
#include <fs/proc.h>
#include <fs/dirent.h>
#include <fs/tty/tty.h>

#include <sched.h>
#include <time.h>

#include <lib/string.h>

static int proc_readdir(struct inode *, struct file *, void *, filldir_t);
static struct file_operations proc_dir_fops = {
	.readdir = proc_readdir,
};

/*
 * procfs inode numbers are synthetic.  The top byte is a node class
 * and the low bytes encode pid/fd where needed.
 */
#define PROC_INO_ROOT	1UL
#define PROC_INO_SELF	2UL
#define PROC_INO_VERSION 3UL
#define PROC_INO_UPTIME	4UL
#define PROC_INO_MOUNTS	5UL
#define PROC_INO_PARTITIONS	6UL

#define PROC_INO_PID	0x01000000UL
#define PROC_INO_FD_DIR	0x02000000UL
#define PROC_INO_FD	0x03000000UL
#define PROC_INO_PID_STAT	0x04000000UL
#define PROC_INO_PID_CMDLINE	0x05000000UL
#define PROC_INO_PID_EXE	0x06000000UL

#define PROC_INO_TYPE_MASK 0xff000000UL
#define PROC_INO_PID_MASK	0x0000ffffUL

static unsigned long proc_pid_ino(pid_t pid)
{
	return PROC_INO_PID | ((unsigned long)pid & PROC_INO_PID_MASK);
}

static pid_t proc_ino_pid(unsigned long ino)
{
	return (pid_t)(ino & PROC_INO_PID_MASK);
}

static unsigned long proc_fd_dir_ino(pid_t pid)
{
	return PROC_INO_FD_DIR | ((unsigned long)pid & PROC_INO_PID_MASK);
}

static unsigned long proc_fd_ino(pid_t pid, int fd)
{
	return PROC_INO_FD |
	       (((unsigned long)pid & PROC_INO_PID_MASK) << 8) |
	       ((unsigned long)fd & 0xff);
}

static unsigned long proc_pid_file_ino(unsigned long type, pid_t pid)
{
	return type | ((unsigned long)pid & PROC_INO_PID_MASK);
}

static pid_t proc_fd_ino_pid(unsigned long ino)
{
	return (pid_t)((ino >> 8) & PROC_INO_PID_MASK);
}

static int proc_fd_ino_fd(unsigned long ino)
{
	return (int)(ino & 0xff);
}

static int proc_name_is(char *name, int len, char *want)
{
	int n = strlen(want);

	return len == n && !memcmp(name, want, n);
}

static struct task_struct *proc_find_task(pid_t pid)
{
	struct task_struct *p;

	for_each_task(p) {
		if (p->pid == pid)
			return p;
	}
	return NULL;
}

static int proc_parse_pid(char *name, int len, pid_t *pid)
{
	int n = 0;

	if (len < 1 || len > 8)
		return 0;
	while (len--) {
		if (*name < '0' || *name > '9')
			return 0;
		n = n * 10 + (*name - '0');
		name++;
	}
	*pid = n;
	return 1;
}

static int proc_parse_fd(char *name, int len, int *fd)
{
	int n = 0;

	if (len < 1 || len > 8)
		return 0;
	while (len--) {
		if (*name < '0' || *name > '9')
			return 0;
		n = n * 10 + (*name - '0');
		name++;
	}
	*fd = n;
	return 1;
}

static mode_t proc_inode_mode(unsigned long ino)
{
	switch (ino) {
	case PROC_INO_SELF:
		return S_IFLNK | 0777;
	case PROC_INO_ROOT:
		return S_IFDIR | 0555;
	case PROC_INO_VERSION:
	case PROC_INO_UPTIME:
	case PROC_INO_MOUNTS:
		return S_IFREG | 0444;
	}

	switch (ino & PROC_INO_TYPE_MASK) {
	case PROC_INO_FD:
	case PROC_INO_PID_EXE:
		return S_IFLNK | 0777;
	case PROC_INO_PID_STAT:
	case PROC_INO_PID_CMDLINE:
		return S_IFREG | 0444;
	case PROC_INO_PID:
	case PROC_INO_FD_DIR:
		return S_IFDIR | 0555;
	default:
		return S_IFREG | 0444;
	}
}

static struct inode *proc_alloc_inode(unsigned long ino, int size,
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
	inode->i_mode = proc_inode_mode(ino);
	inode->i_nlink = S_ISDIR(inode->i_mode) ? 2 : 1;
	inode->i_size = size;
	inode->i_block_size = 512;
	inode->i_op = iop;
	inode->i_fop = fop;
	inode->i_atime = inode->i_mtime = inode->i_ctime = 0;
	return inode;
}

static char proc_task_state(struct task_struct *task)
{
	switch (task->state) {
	case TASK_RUNNING:
		return 'R';
	case TASK_INTERRUPTIBLE:
		return 'S';
	case TASK_UNINTERRUPTIBLE:
		return 'D';
	case TASK_ZOMBIE:
		return 'Z';
	case TASK_STOPPED:
		return 'T';
	default:
		return '?';
	}
}

static int proc_pid_stat_content(pid_t pid, char *buf)
{
	struct task_struct *task = proc_find_task(pid);
	int n;

	if (!task)
		return -ENOENT;

	n = sprintk(buf, "%d (%s) %c", task->pid, task->comm,
		    proc_task_state(task));
	n += sprintk(buf + n, " %d %d %d %d %d 0 0 0 0 0",
		     task->p_pptr ? task->p_pptr->pid : 0,
		     task->pgrp, task->session,
		     task->tty ? task->tty->dev : 0,
		     task->tty ? task->tty->pgrp : 0);
	n += sprintk(buf + n, " %d %d %d %d %d %d 1",
		     (int)task->utime, (int)task->stime,
		     (int)task->utime_children, (int)task->stime_children,
		     task->priority, NZERO - task->priority);
	n += sprintk(buf + n, " 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
	n += sprintk(buf + n, " %d", task->exit_signal);
	n += sprintk(buf + n, " 0 0 0 0 0 0 0 0\n");
	return n;
}

static int proc_pid_cmdline_content(pid_t pid, char *buf)
{
	struct task_struct *task = proc_find_task(pid);

	if (!task)
		return -ENOENT;

	return sprintk(buf, "%s", task->comm);
}

static int proc_blkdev_name(unsigned int major, unsigned int minor,
			    char *buf, size_t size)
{
	(void)size;

	if (major == RD_MAJOR)
		return sprintk(buf, "rd%u", minor);
	if (major == HD_MAJOR) {
		unsigned int disk = minor / 17;
		unsigned int part = minor % 17;

		if (!part)
			return sprintk(buf, "hd%c", 'a' + disk);
		return sprintk(buf, "hd%c%u", 'a' + disk, part);
	}
	return -EINVAL;
}

static int proc_partitions_content(char *buf, size_t size)
{
	unsigned int major;
	unsigned int minor;
	int n;

	n = sprintk(buf, "major minor  #blocks  name\n\n");
	for (major = 1; major <= HD_MAJOR; major += 2) {
		unsigned int max_minor = major == RD_MAJOR ? 6 : 6 * 17;
		char name[16];

		for (minor = 0; minor < max_minor; minor++) {
			struct blkdev_info info;
			dev_t dev = MKDEV(major, minor);
			int name_len;

			if (blkdev_get_info(dev, &info))
				continue;
			name_len = proc_blkdev_name(major, minor, name,
						    sizeof(name));
			if (name_len < 0)
				continue;
			if ((size_t)n + 40 >= size)
				return n;
			n += sprintk(buf + n, " %u %u %u %s\n", major, minor,
				     (unsigned int)info.nr *
				     info.sector_size / 1024,
				     name);
		}
	}
	return n;
}

static int proc_file_content(unsigned long ino, char *buf, size_t size)
{
	(void)size;

	switch (ino) {
	case PROC_INO_VERSION:
		return sprintk(buf, "Mario OS version 1.0\n");
	case PROC_INO_UPTIME: {
		long up = jiffies / HZ;
		long up_cs = (jiffies % HZ) * 100 / HZ;

		return sprintk(buf, "%d.%d 0.00\n", (int)up, (int)up_cs);
	}
	case PROC_INO_MOUNTS:
		return sprintk(buf, "mariofs / mariofs rw 0 0\n");
	case PROC_INO_PARTITIONS:
		return proc_partitions_content(buf, size);
	default:
		break;
	}

	if ((ino & PROC_INO_TYPE_MASK) == PROC_INO_PID_STAT)
		return proc_pid_stat_content(proc_ino_pid(ino), buf);
	if ((ino & PROC_INO_TYPE_MASK) == PROC_INO_PID_CMDLINE)
		return proc_pid_cmdline_content(proc_ino_pid(ino), buf);

	return -EINVAL;
}

static int proc_file_read(struct inode *inode, struct file *file,
			  char *buf, int count)
{
	char content[512];
	int size = proc_file_content(inode->i_ino, content, sizeof(content));

	if (size < 0)
		return size;
	if (file->f_pos >= size)
		return 0;
	if (count > size - file->f_pos)
		count = size - file->f_pos;
	memcpy_tofs(buf, content + file->f_pos, count);
	file->f_pos += count;
	return count;
}

static struct file_operations proc_file_fops = {
	.read = proc_file_read,
};

static int proc_link_target(unsigned long ino, char *buf, size_t size)
{
	(void)size;

	if (ino == PROC_INO_SELF)
		return sprintk(buf, "/proc/%d", current->pid);

	if ((ino & PROC_INO_TYPE_MASK) == PROC_INO_PID_EXE) {
		struct task_struct *task = proc_find_task(proc_ino_pid(ino));

		if (!task)
			return -ENOENT;
		return sprintk(buf, "/bin/%s", task->comm);
	}

	if ((ino & PROC_INO_TYPE_MASK) != PROC_INO_FD)
		return -EINVAL;

	pid_t pid = proc_fd_ino_pid(ino);
	int fd = proc_fd_ino_fd(ino);
	struct task_struct *task = proc_find_task(pid);
	if (!task)
		return -ENOENT;
	if (fd >= NR_OPEN)
		return -ENOENT;

	struct file *file = task->files->fd[fd];
	if (!file || !file->f_inode)
		return -ENOENT;

	struct inode *inode = file->f_inode;
	if (S_ISCHR(inode->i_mode)) {
		dev_t dev = inode->i_rdev;
		unsigned int major = MAJOR(dev);
		unsigned int minor = MINOR(dev);

		if (major == TTY_MAJOR) {
			if (minor == TTY_MINOR)
				return sprintk(buf, "/dev/tty");
			if (minor == SYSTEM_CONSOLE_MINOR)
				return sprintk(buf, "/dev/console");
			if (minor == TTY_MINOR_0)
				return sprintk(buf, "/dev/tty0");
			if (TTY_MINOR_1 <= minor &&
			    minor < TTY_MINOR_1 + NUM_CONSOLE)
				return sprintk(buf, "/dev/tty%d",
					       minor - TTY_MINOR_1 + 1);
			if (SERIAL_MINOR_0 <= minor &&
			    minor < SERIAL_MINOR_0 + NUM_SERIAL)
				return sprintk(buf, "/dev/ttyS%d",
					       minor - SERIAL_MINOR_0);
		}
	}

	if (S_ISREG(inode->i_mode))
		return sprintk(buf, "/%s", MARIO_INODE_NAME(inode));

	return sprintk(buf, "anon_inode:[%u]", (unsigned int)inode->i_ino);
}

static int proc_readlink(struct inode *inode, char *buf, int count)
{
	char target[64];
	int size = proc_link_target(inode->i_ino, target, sizeof(target));

	if (size < 0) {
		iput(inode);
		return size;
	}

	memcpy_tofs(buf, target, MIN(count, size));
	iput(inode);
	return size;
}

static int proc_follow_link(struct inode *dir, struct inode *inode,
			    int flags, struct inode **res_inode)
{
	*res_inode = NULL;
	if (!dir) {
		dir = current->fs->root;
		iref(dir);
	}
	if (!inode) {
		iput(dir);
		return -ENOENT;
	}
	if (!S_ISLNK(inode->i_mode)) {
		iput(dir);
		*res_inode = inode;
		return 0;
	}
	if (current->link_count > MAX_LINK_COUNT) {
		iput(dir);
		iput(inode);
		return -ELOOP;
	}

	char target[64];
	int size = proc_link_target(inode->i_ino, target, sizeof(target));
	if (size < 0) {
		iput(dir);
		iput(inode);
		return size;
	}

	iput(inode);
	current->link_count++;
	int error = open_namei(target, flags, res_inode, dir);
	current->link_count--;
	return error;
}

static struct inode_operations proc_link_iops = {
	.readlink = proc_readlink,
	.follow_link = proc_follow_link,
};

static struct inode *proc_make_file(unsigned long ino)
{
	char content[512];
	int size = proc_file_content(ino, content, sizeof(content));

	if (size < 0)
		return NULL;
	return proc_alloc_inode(ino, size, NULL, &proc_file_fops);
}

static struct inode *proc_make_link(unsigned long ino)
{
	char target[64];
	int size = proc_link_target(ino, target, sizeof(target));

	if (size < 0)
		return NULL;
	return proc_alloc_inode(ino, size, &proc_link_iops, NULL);
}

static struct inode *proc_make_dir(unsigned long ino)
{
	return proc_alloc_inode(ino, 0, &proc_dir_iops, &proc_dir_fops);
}

static int proc_lookup_child(unsigned long dir_ino, char *name, int len,
			     struct inode **res)
{
	*res = NULL;

	if (dir_ino == PROC_INO_ROOT) {
		if (proc_name_is(name, len, "self"))
			*res = proc_make_link(PROC_INO_SELF);
		else if (proc_name_is(name, len, "version"))
			*res = proc_make_file(PROC_INO_VERSION);
		else if (proc_name_is(name, len, "uptime"))
			*res = proc_make_file(PROC_INO_UPTIME);
		else if (proc_name_is(name, len, "mounts"))
			*res = proc_make_file(PROC_INO_MOUNTS);
		else if (proc_name_is(name, len, "partitions"))
			*res = proc_make_file(PROC_INO_PARTITIONS);
		else {
			pid_t pid;
			if (proc_parse_pid(name, len, &pid) &&
			    proc_find_task(pid))
				*res = proc_make_dir(proc_pid_ino(pid));
		}
		return *res ? 0 : -ENOENT;
	}

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_PID) {
		pid_t pid = proc_ino_pid(dir_ino);
		if (proc_name_is(name, len, "fd"))
			*res = proc_make_dir(proc_fd_dir_ino(pid));
		else if (proc_name_is(name, len, "stat"))
			*res = proc_make_file(proc_pid_file_ino(PROC_INO_PID_STAT, pid));
		else if (proc_name_is(name, len, "cmdline"))
			*res = proc_make_file(proc_pid_file_ino(PROC_INO_PID_CMDLINE, pid));
		else if (proc_name_is(name, len, "exe"))
			*res = proc_make_link(proc_pid_file_ino(PROC_INO_PID_EXE, pid));
		return *res ? 0 : -ENOENT;
	}

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_FD_DIR) {
		pid_t pid = proc_ino_pid(dir_ino);
		struct task_struct *task = proc_find_task(pid);
		int fd;

		if (task && proc_parse_fd(name, len, &fd) &&
		    fd < NR_OPEN && task->files->fd[fd] &&
		    task->files->fd[fd]->f_inode)
			*res = proc_make_link(proc_fd_ino(pid, fd));
		return *res ? 0 : -ENOENT;
	}

	return -ENOTDIR;
}

static int proc_parent(unsigned long dir_ino, struct inode **res)
{
	*res = NULL;

	if (dir_ino == PROC_INO_ROOT) {
		*res = current->fs->root;
		iref(*res);
		return 0;
	}

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_PID) {
		*res = proc_make_dir(PROC_INO_ROOT);
		return *res ? 0 : -ENOMEM;
	}

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_FD_DIR) {
		pid_t pid = proc_ino_pid(dir_ino);
		*res = proc_make_dir(proc_pid_ino(pid));
		return *res ? 0 : -ENOMEM;
	}

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_PID_STAT ||
	    (dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_PID_CMDLINE ||
	    (dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_PID_EXE) {
		pid_t pid = proc_ino_pid(dir_ino);
		*res = proc_make_dir(proc_pid_ino(pid));
		return *res ? 0 : -ENOMEM;
	}

	return -ENOENT;
}

static int proc_lookup(struct inode *dir, char *name, int len,
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
		error = proc_parent(dir->i_ino, res);
		iput(dir);
		return error;
	}

	error = proc_lookup_child(dir->i_ino, name, len, res);
	iput(dir);
	return error;
}

struct inode_operations proc_dir_iops = {
	.lookup = proc_lookup,
};

static int proc_root_entry(int index, char *name, size_t size, ino_t *ino,
			   unsigned char *type)
{
	struct task_struct *task;

	(void)size;

	switch (index) {
	case 0:
		sprintk(name, "%s", "self");
		*ino = PROC_INO_SELF;
		*type = DT_LNK;
		return 0;
	case 1:
		sprintk(name, "%s", "version");
		*ino = PROC_INO_VERSION;
		*type = DT_REG;
		return 0;
	case 2:
		sprintk(name, "%s", "uptime");
		*ino = PROC_INO_UPTIME;
		*type = DT_REG;
		return 0;
	case 3:
		sprintk(name, "%s", "mounts");
		*ino = PROC_INO_MOUNTS;
		*type = DT_REG;
		return 0;
	case 4:
		sprintk(name, "%s", "partitions");
		*ino = PROC_INO_PARTITIONS;
		*type = DT_REG;
		return 0;
	default:
		index -= 5;
		for_each_task(task) {
			if (index-- == 0) {
				sprintk(name, "%d", task->pid);
				*ino = proc_pid_ino(task->pid);
				*type = DT_DIR;
				return 0;
			}
		}
		return -ENOENT;
	}
}

static int proc_pid_entry(pid_t pid, int index, char *name, size_t size,
			  ino_t *ino, unsigned char *type)
{
	(void)size;

	switch (index) {
	case 0:
		sprintk(name, "%s", "cmdline");
		*ino = proc_pid_file_ino(PROC_INO_PID_CMDLINE, pid);
		*type = DT_REG;
		return 0;
	case 1:
		sprintk(name, "%s", "exe");
		*ino = proc_pid_file_ino(PROC_INO_PID_EXE, pid);
		*type = DT_LNK;
		return 0;
	case 2:
		sprintk(name, "%s", "fd");
		*ino = proc_fd_dir_ino(pid);
		*type = DT_DIR;
		return 0;
	case 3:
		sprintk(name, "%s", "stat");
		*ino = proc_pid_file_ino(PROC_INO_PID_STAT, pid);
		*type = DT_REG;
		return 0;
	default:
		return -ENOENT;
	}
}

static int proc_fd_dir_entry(pid_t pid, int index, char *name, size_t size,
			     ino_t *ino, unsigned char *type)
{
	struct task_struct *task = proc_find_task(pid);
	int seen = 0;
	int fd;

	(void)size;

	if (!task)
		return -ENOENT;

	for (fd = 0; fd < NR_OPEN; fd++) {
		if (!task->files->fd[fd])
			continue;
		if (seen++ == index) {
			sprintk(name, "%d", fd);
			*ino = proc_fd_ino(pid, fd);
			*type = DT_LNK;
			return 0;
		}
	}
	return -ENOENT;
}

static int proc_get_entry(unsigned long dir_ino, int index, char *name,
			  size_t size, ino_t *ino, unsigned char *type)
{
	if (dir_ino == PROC_INO_ROOT)
		return proc_root_entry(index, name, size, ino, type);

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_PID)
		return proc_pid_entry(proc_ino_pid(dir_ino), index, name,
				      size, ino, type);

	if ((dir_ino & PROC_INO_TYPE_MASK) == PROC_INO_FD_DIR)
		return proc_fd_dir_entry(proc_ino_pid(dir_ino), index, name,
					 size, ino, type);

	return -ENOTDIR;
}

static int proc_readdir(struct inode *dir, struct file *file, void *dirent,
			filldir_t fn)
{
	char name[16];
	ino_t ino;
	unsigned char type;
	int index = file->f_pos;

	if (proc_get_entry(dir->i_ino, index, name, sizeof(name),
			   &ino, &type))
		return 0;

	if (fn(dirent, name, strlen(name), index, ino, type))
		return 0;

	file->f_pos = index + 1;
	return 0;
}

struct inode *proc_get_root(void)
{
	return proc_make_dir(PROC_INO_ROOT);
}
