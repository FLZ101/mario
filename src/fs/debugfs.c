#include <fs/fs.h>
#include <fs/debugfs.h>
#include <fs/dirent.h>
#include <fs/ahci.h>

#include <syscall_trace.h>

#include <mm/kmalloc.h>

#include <lib/string.h>

#define DEBUGFS_INO_ROOT	1UL
#define DEBUGFS_INO_SYSCALLS	4UL
#define DEBUGFS_INO_TRACE_PID	5UL
#define DEBUGFS_INO_AHCI	6UL
#define DEBUGFS_INO_FILEMAP	7UL

#define DEBUGFS_SYSCALLS_BUF	8192
#define DEBUGFS_AHCI_BUF	1024
#define DEBUGFS_FILEMAP_BUF	1024

static struct file_operations debugfs_dir_fops;

static struct super_block *debugfs_sb;

static int debugfs_name_is(char *name, int len, char *want)
{
	int n = strlen(want);

	return len == n && !memcmp(name, want, n);
}

static mode_t debugfs_inode_mode(unsigned long ino)
{
	if (ino == DEBUGFS_INO_SYSCALLS || ino == DEBUGFS_INO_TRACE_PID ||
	    ino == DEBUGFS_INO_AHCI || ino == DEBUGFS_INO_FILEMAP)
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

	inode->i_dev = debugfs_sb ? debugfs_sb->s_dev : 0;
	inode->i_ino = ino;
	inode->i_sb = debugfs_sb;
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

static int debugfs_sput_u64(char *buf, size_t size, uint64_t value)
{
	char tmp[20];
	int i = sizeof(tmp);
	int len = 0;

	if (!size)
		return 0;
	if (!value)
		return sprintkn(buf, size, "0");

	while (value) {
		tmp[--i] = "0123456789"[value % 10];
		value /= 10;
	}
	while (i < sizeof(tmp) && len < size - 1)
		buf[len++] = tmp[i++];
	buf[len] = '\0';
	return len;
}

/* Print a TSC cycle count as milliseconds with three fractional digits. */
static int debugfs_sput_cycles_ms(char *buf, size_t size, uint64_t cycles,
				     uint64_t cycles_per_ms)
{
	uint64_t whole, frac;
	int n = 0;

	if (!size)
		return 0;
	if (!cycles_per_ms)
		return sprintkn(buf, size, "0.000");

	whole = cycles / cycles_per_ms;
	frac = (cycles % cycles_per_ms) * 1000 / cycles_per_ms;

	n += debugfs_sput_u64(buf + n, size - n, whole);
	n += sprintkn(buf + n, size - n, ".");
	if (frac < 100)
		n += sprintkn(buf + n, size - n, "0");
	if (frac < 10)
		n += sprintkn(buf + n, size - n, "0");
	n += debugfs_sput_u64(buf + n, size - n, frac);
	return n;
}

static int debugfs_syscalls_content(char *buf, size_t size)
{
	char line[192];
	int n = 0;
	uint64_t cycles_per_ms;
	unsigned long elapsed_ms, user_ms, kernel_ms;
	unsigned long syscall_ms, kernel_other_ms, offcpu_ms;
	unsigned int nr;

	if (!size)
		return 0;

	cycles_per_ms = syscall_trace_cycles_per_ms();

	/*
	 * Partition the traced wall time into user, syscall, other-kernel and
	 * off-CPU time.  syscall_ms is precise TSC accounting; user/kernel use
	 * the PIT tick counters, and offcpu is the remainder.
	 */
	elapsed_ms = syscall_trace_elapsed_ms();
	user_ms = syscall_trace_user_ms();
	kernel_ms = syscall_trace_kernel_ms();
	syscall_ms = syscall_trace_total_ms();
	kernel_other_ms = kernel_ms > syscall_ms ? kernel_ms - syscall_ms : 0;
	offcpu_ms = elapsed_ms > user_ms + kernel_ms
			    ? elapsed_ms - user_ms - kernel_ms
			    : 0;

	n += sprintkn(buf, size, "elapsed_ms %u\n",
		      (unsigned int)elapsed_ms);
	n += sprintkn(buf + n, size - n, "user_ms %u\n",
		      (unsigned int)user_ms);
	n += sprintkn(buf + n, size - n, "kernel_ms %u\n",
		      (unsigned int)kernel_ms);
	n += sprintkn(buf + n, size - n, "syscall_ms %u\n",
		      (unsigned int)syscall_ms);
	n += sprintkn(buf + n, size - n, "kernel_other_ms %u\n",
		      (unsigned int)kernel_other_ms);
	n += sprintkn(buf + n, size - n, "offcpu_ms %u\n",
		      (unsigned int)offcpu_ms);
	n += sprintkn(buf + n, size - n,
		      "syscall name calls total_ms avg_ms max_ms\n");

	for (nr = 0; nr < NR_SYSCALLS; nr++) {
		struct syscall_trace_stat stat;
		uint64_t avg;
		int len = 0;

		syscall_trace_stat_get(nr, &stat);
		if (!stat.calls)
			continue;

		avg = stat.cycles / stat.calls;
		len += sprintkn(line + len, sizeof(line) - len, "%u ", nr);
		if (syscall_trace_name(nr))
			len += sprintkn(line + len, sizeof(line) - len,
					"%s ", syscall_trace_name(nr));
		else
			len += sprintkn(line + len, sizeof(line) - len, "- ");
		len += debugfs_sput_u64(line + len, sizeof(line) - len,
					stat.calls);
		len += sprintkn(line + len, sizeof(line) - len, " ");
		len += debugfs_sput_cycles_ms(line + len, sizeof(line) - len,
					     stat.cycles, cycles_per_ms);
		len += sprintkn(line + len, sizeof(line) - len, " ");
		len += debugfs_sput_cycles_ms(line + len, sizeof(line) - len,
					     avg, cycles_per_ms);
		len += sprintkn(line + len, sizeof(line) - len, " ");
		len += debugfs_sput_cycles_ms(line + len, sizeof(line) - len,
					     stat.max_cycles, cycles_per_ms);
		len += sprintkn(line + len, sizeof(line) - len, "\n");

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
	return sprintkn(buf, size, "%d\n", syscall_trace_get_pid());
}

static int debugfs_ahci_counter_content(char *buf, size_t size, char *name,
					struct ahci_profile_counter *counter,
					uint64_t cycles_per_ms)
{
	int n = 0;

	n += sprintkn(buf + n, size - n, "%s commands ", name);
	n += debugfs_sput_u64(buf + n, size - n, counter->commands);
	n += sprintkn(buf + n, size - n, " sectors ");
	n += debugfs_sput_u64(buf + n, size - n, counter->sectors);
	n += sprintkn(buf + n, size - n, " wait_ms ");
	n += debugfs_sput_cycles_ms(buf + n, size - n, counter->wait_cycles,
				     cycles_per_ms);
	n += sprintkn(buf + n, size - n, "\n");
	return n;
}

static int debugfs_ahci_content(char *buf, size_t size)
{
	static char *command_names[AHCI_PROFILE_COMMAND_COUNT] = {
		"read", "write", "identify", "flush", "other",
	};
	static char *size_names[AHCI_PROFILE_SIZE_COUNT] = {
		"no_data", "1_sector", "2_to_8_sectors", "9_to_64_sectors",
		"over_64_sectors",
	};
	struct ahci_profile profile;
	uint64_t cycles_per_ms = syscall_trace_cycles_per_ms();
	int index;
	int n = 0;

	ahci_profile_get(&profile);
	n += debugfs_ahci_counter_content(buf + n, size - n, "total",
					&profile.total, cycles_per_ms);
	n += sprintkn(buf + n, size - n, "by_command\n");
	for (index = 0; index < AHCI_PROFILE_COMMAND_COUNT; index++)
		n += debugfs_ahci_counter_content(buf + n, size - n,
						command_names[index],
						&profile.command[index], cycles_per_ms);
	n += sprintkn(buf + n, size - n, "by_size\n");
	for (index = 0; index < AHCI_PROFILE_SIZE_COUNT; index++)
		n += debugfs_ahci_counter_content(buf + n, size - n,
						size_names[index],
						&profile.size[index], cycles_per_ms);
	return n;
}

static int debugfs_filemap_content(char *buf, size_t size)
{
	struct filemap_stats stats;
	int n = 0;

	filemap_get_stats(&stats);
	n += sprintkn(buf + n, size - n, "cached_pages %lu\n", stats.cached_pages);
	n += sprintkn(buf + n, size - n, "read_acquires %lu\n", stats.read_acquires);
	n += sprintkn(buf + n, size - n, "mmap_acquires %lu\n", stats.mmap_acquires);
	n += sprintkn(buf + n, size - n, "read_hits %lu\n", stats.read_hits);
	n += sprintkn(buf + n, size - n, "mmap_hits %lu\n", stats.mmap_hits);
	n += sprintkn(buf + n, size - n, "read_hits_mmap_filled %lu\n",
		      stats.read_hits_mmap_filled);
	n += sprintkn(buf + n, size - n, "mmap_hits_read_filled %lu\n",
		      stats.mmap_hits_read_filled);
	n += sprintkn(buf + n, size - n, "mmap_zero_hits %lu\n",
		      stats.mmap_zero_hits);
	n += sprintkn(buf + n, size - n, "misses %lu\n", stats.misses);
	n += sprintkn(buf + n, size - n, "loader_starts %lu\n",
		      stats.loader_starts);
	n += sprintkn(buf + n, size - n, "loader_errors %lu\n",
		      stats.loader_errors);
	n += sprintkn(buf + n, size - n, "waiters %lu\n", stats.waiters);
	n += sprintkn(buf + n, size - n, "invalidations %lu\n",
		      stats.invalidations);
	n += sprintkn(buf + n, size - n, "evictions %lu\n", stats.evictions);
	n += sprintkn(buf + n, size - n, "budget_full %lu\n",
		      stats.budget_full);
	n += sprintkn(buf + n, size - n, "uncached_bytes %lu\n",
		      stats.uncached_bytes);
	n += sprintkn(buf + n, size - n, "temporary_fallbacks %lu\n",
		      stats.temporary_fallbacks);
	return n;
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

static int debugfs_is_reset(char *buf, int len)
{
	if (len == 6 && buf[5] == '\n')
		len--;
	return len == 5 && !memcmp(buf, "reset", 5);
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
	if (inode->i_ino == DEBUGFS_INO_AHCI) {
		char tmp[DEBUGFS_AHCI_BUF];

		size = debugfs_ahci_content(tmp, sizeof(tmp));
		if (file->f_pos >= size)
			return 0;
		if (count > size - file->f_pos)
			count = size - file->f_pos;
		memcpy_tofs(buf, tmp + file->f_pos, count);
		file->f_pos += count;
		return count;
	}
	if (inode->i_ino == DEBUGFS_INO_FILEMAP) {
		char tmp[DEBUGFS_FILEMAP_BUF];

		size = debugfs_filemap_content(tmp, sizeof(tmp));
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
	if (inode->i_ino == DEBUGFS_INO_AHCI) {
		ahci_profile_reset();
		return count;
	}
	if (inode->i_ino == DEBUGFS_INO_FILEMAP) {
		if (count <= 0 || count > sizeof(tmp))
			return -EINVAL;
		memcpy_fromfs(tmp, buf, count);
		if (!debugfs_is_reset(tmp, count))
			return -EINVAL;
		filemap_reset_stats();
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
	else if (ino == DEBUGFS_INO_AHCI)
		size = DEBUGFS_AHCI_BUF;
	else if (ino == DEBUGFS_INO_FILEMAP)
		size = DEBUGFS_FILEMAP_BUF;
	return debugfs_alloc_inode(ino, size, NULL, &debugfs_file_fops);
}

static int debugfs_lookup_child(unsigned long dir_ino, char *name, int len,
				struct inode **res)
{
	*res = NULL;

	if (dir_ino == DEBUGFS_INO_ROOT) {
		if (debugfs_name_is(name, len, "syscalls"))
			*res = debugfs_make_file(DEBUGFS_INO_SYSCALLS);
		else if (debugfs_name_is(name, len, "trace_pid"))
			*res = debugfs_make_file(DEBUGFS_INO_TRACE_PID);
		else if (debugfs_name_is(name, len, "ahci"))
			*res = debugfs_make_file(DEBUGFS_INO_AHCI);
		else if (debugfs_name_is(name, len, "filemap"))
			*res = debugfs_make_file(DEBUGFS_INO_FILEMAP);
		return *res ? 0 : -ENOENT;
	}

	return -ENOTDIR;
}

static int debugfs_parent(unsigned long dir_ino, struct inode **res)
{
	*res = NULL;

	if (dir_ino == DEBUGFS_INO_ROOT) {
		if (!debugfs_sb || !debugfs_sb->s_covered)
			return -ENOENT;
		*res = debugfs_sb->s_covered;
		iref(*res);
		return 0;
	}

	if (dir_ino == DEBUGFS_INO_SYSCALLS || dir_ino == DEBUGFS_INO_TRACE_PID ||
	    dir_ino == DEBUGFS_INO_AHCI || dir_ino == DEBUGFS_INO_FILEMAP) {
		*res = debugfs_make_dir(DEBUGFS_INO_ROOT);
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
	if (dir_ino == DEBUGFS_INO_ROOT) {
		switch (index) {
		case 0:
			sprintkn(name, size, "%s", "syscalls");
			*ino = DEBUGFS_INO_SYSCALLS;
			*type = DT_REG;
			return 0;
		case 1:
			sprintkn(name, size, "%s", "trace_pid");
			*ino = DEBUGFS_INO_TRACE_PID;
			*type = DT_REG;
			return 0;
		case 2:
			sprintkn(name, size, "%s", "ahci");
			*ino = DEBUGFS_INO_AHCI;
			*type = DT_REG;
			return 0;
		case 3:
			sprintkn(name, size, "%s", "filemap");
			*ino = DEBUGFS_INO_FILEMAP;
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
	if (!debugfs_sb || !debugfs_sb->s_mounted)
		return NULL;
	iref(debugfs_sb->s_mounted);
	return debugfs_sb->s_mounted;
}

static struct super_operations debugfs_sops;

static struct super_block *debugfs_read_super(struct super_block *sb)
{
	debugfs_sb = sb;
	sb->s_magic = DEBUGFS_SUPER_MAGIC;
	sb->s_block_size = 4096;
	sb->s_op = &debugfs_sops;
	sb->s_mounted = debugfs_make_dir(DEBUGFS_INO_ROOT);
	if (!sb->s_mounted)
		return NULL;
	sb->s_mounted->i_sb = sb;
	return sb;
}

struct file_system_type debugfs = {
	.read_super = debugfs_read_super,
	.list = LIST_HEAD_INIT(debugfs.list),
	.name = "debugfs",
	.fs_flags = FS_NODEV,
};
