#include <fs/fs.h>

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

/*
 * man 4 random
 *
 * When read, the /dev/urandom device returns random bytes using a pseudorandom number
 * generator seeded from the entropy pool.
 *
 * Writing to /dev/random or /dev/urandom will update the entropy pool with the data written.
 */

extern volatile time_t boot_time_sec;

static uint32_t rand_state;

static void rand_init(void)
{
    assert(boot_time_sec > 0);
    rand_state = boot_time_sec;
}

static uint32_t rand() {
    uint32_t x = rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rand_state = x;
}

static uint8_t rand_byte() {
	static int i = 4;
	static uint8_t x[4];

	if (i == 4) {
		*(uint32_t *)x = rand();
		i = 0;
	}
	return x[i++];
}

static int __rand_read(char *buf, int count)
{
	int left;
	for (left = count; left > 0; left--) {
		put_fs_byte(rand_byte(), buf);
		buf++;
	}
	return count;
}

static int rand_read(struct inode *i, struct file *f, char *buf, int count)
{
	return __rand_read(buf, count);
}

#define GRND_NONBLOCK	0x0001
#define GRND_RANDOM	0x0002
#define GRND_INSECURE	0x0004

ssize_t sys_getrandom(void *buf, size_t len, unsigned int flags)
{
	int err = verify_area(VERIFY_WRITE, buf, len);
	if (err)
		return err;
	__rand_read(buf, len);
    return len;
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
