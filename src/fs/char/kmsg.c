#include <fs/fs.h>

#include <logbuf.h>

#include <lib/string.h>

int kmsg_open(struct inode *i, struct file *f)
{
	f->f_pos = logbuf_oldest_seq();
	return 0;
}

int kmsg_read(struct inode *i, struct file *f, char *buf, int count)
{
	char tmp[LOG_LINE_MAX + 64];
	uint32_t next_seq;
	size_t size;
	int ret;

	size = count < sizeof(tmp) ? count : sizeof(tmp);
	ret = logbuf_read_wait((uint32_t)f->f_pos, tmp, size, &next_seq,
			       f->f_flags & O_NONBLOCK);
	if (ret <= 0) {
		if (ret == -EPIPE)
			f->f_pos = logbuf_oldest_seq();
		return ret;
	}

	memcpy_tofs(buf, tmp, ret);
	f->f_pos = next_seq;
	return ret;
}

int kmsg_write(struct inode *i, struct file *f, char *buf, int count)
{
	char tmp[LOG_LINE_MAX + 1];
	char *text;
	int len;
	int prio = LOG_INFO;

	if (count <= 0)
		return 0;

	len = count < LOG_LINE_MAX ? count : LOG_LINE_MAX;
	memcpy_fromfs(tmp, buf, len);
	tmp[len] = '\0';

	text = tmp;
	if (text[0] == '<') {
		char *p = text + 1;

		prio = 0;
		while ('0' <= *p && *p <= '9') {
			prio = prio * 10 + *p - '0';
			p++;
		}
		if (*p == '>') {
			text = p + 1;
			len -= text - tmp;
		} else {
			prio = LOG_INFO;
		}
	}

	if (len > 0 && text[len - 1] == '\n')
		len--;

	logbuf_store(prio, text, len);
	return count;
}

int kmsg_lseek(struct inode *i, struct file *f, off_t offset, int origin)
{
	switch (origin) {
	case SEEK_SET:
		if (offset)
			return -EINVAL;
		f->f_pos = logbuf_oldest_seq();
		return f->f_pos;
	case SEEK_END:
		if (offset)
			return -EINVAL;
		f->f_pos = logbuf_next_seq();
		return f->f_pos;
	default:
		return -EINVAL;
	}
}

struct file_operations kmsg_fops = {
	.open = kmsg_open,
	.read = kmsg_read,
	.write = kmsg_write,
	.lseek = kmsg_lseek,
};
