#include <fs/fs.h>

#define RANDOM_MAX_READ 0x7fffffffU

extern struct file_operations kmsg_fops;

/*
 * man 4 null
 *
 * Data written to the /dev/null and /dev/zero special files is discarded.
 *
 * Reads from /dev/null always return end of file (i.e., read(2) returns 0),
 * whereas reads from /dev/zero always return bytes containing zero ('\0' characters).
 */

int zero_read(struct inode *i, struct file *f, char *buf, int count)
{
	int left;
	for (left = count; left > 0; left--) {
		put_fs_byte(0, buf);
		buf++;
	}
	return count;
}

int zero_write(struct inode *i, struct file *f, char *buf, int count)
{
	return count;
}

struct file_operations zero_fops = {
	.read = zero_read,
	.write = zero_write
};

int null_read(struct inode *i, struct file *f, char *buf, int count)
{
	return 0;
}

struct file_operations null_fops = {
	.read = null_read,
	.write = zero_write
};

/*
 * man 4 full
 *
 * Writes to the /dev/full device fail with an ENOSPC error.
 * This can be used to test how a program handles disk-full errors.
 *
 * Reads from the /dev/full device will return \0 characters.
 *
 * Seeks on /dev/full will always succeed.
 */

int full_write(struct inode *i, struct file *f, char *buf, int count)
{
	return -ENOSPC;
}

struct file_operations full_fops = {
	.read = zero_read,
	.write = full_write,
};

static int have_rdrand;

static void rand_init(void)
{
	unsigned int leaf, features;
	asm volatile("cpuid" : "=a"(leaf) : "0"(0) : "ebx", "ecx", "edx");
	if (leaf < 1)
		return;
	asm volatile("cpuid" : "=a"(leaf), "=c"(features) : "0"(1) : "ebx", "edx");
	have_rdrand = (features & (1U << 30)) != 0;
}

static int rdrand_word(uint32_t *word)
{
	for (int attempt = 0; attempt < 10; ++attempt) {
		unsigned char ready;
		asm volatile("rdrand %0; setc %1" : "=r"(*word), "=qm"(ready) :: "cc");
		if (ready)
			return 0;
	}
	return -EAGAIN;
}

static int random_read(char *buf, size_t count)
{
	if (!count)
		return 0;
	if (!have_rdrand)
		return -ENODEV;

	size_t written = 0;
	while (written < count) {
		uint32_t word;
		int error = rdrand_word(&word);
		if (error)
			return written ? (int)written : error;
		size_t size = MIN(sizeof(word), count - written);
		memcpy_tofs(buf + written, &word, size);
		written += size;
	}
	return written;
}

static int rand_read(struct inode *i, struct file *f, char *buf, int count)
{
	return random_read(buf, MIN((unsigned int)count, RANDOM_MAX_READ));
}

#define GRND_NONBLOCK	0x0001
#define GRND_RANDOM	0x0002
#define GRND_INSECURE	0x0004

ssize_t sys_getrandom(void *buf, size_t len, unsigned int flags)
{
	if ((flags & ~(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) ||
	    ((flags & GRND_RANDOM) && (flags & GRND_INSECURE)))
		return -EINVAL;
	if (!len)
		return 0;
	len = MIN(len, RANDOM_MAX_READ);
	int err = verify_area(VERIFY_WRITE, buf, len);
	if (err)
		return err;
	return random_read(buf, len);
}

struct file_operations rand_fops = {
	.read = rand_read,
	.write = zero_write,
};

int mem_open(struct inode *i, struct file *f)
{
	switch (MINOR(i->i_rdev)) {
	case MEM_MINOR_ZERO:
		f->f_op = &zero_fops;
		return 0;
	case MEM_MINOR_NULL:
		f->f_op = &null_fops;
		return 0;
	case MEM_MINOR_FULL:
		f->f_op = &full_fops;
		return 0;
	case MEM_MINOR_RANDOM:
	case MEM_MINOR_URANDOM:
		f->f_op = &rand_fops;
		return 0;
	case MEM_MINOR_KMSG:
		f->f_op = &kmsg_fops;
		return 0;
	default:
		return -ENODEV;
	}
}

struct file_operations mem_fops = {
    .open = mem_open,
};

void mem_init(void)
{
	rand_init();
	register_chrdev(MEM_MAJOR, &mem_fops);
}
