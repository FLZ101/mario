#include <fs/fs.h>
#include <fs/ioctl.h>

static int blkdev_file_open(struct inode *inode, struct file *file)
{
	struct blkdev_info info;

	return blkdev_get_info(inode->i_rdev, &info);
}

static int blkdev_file_lseek(struct inode *inode, struct file *file,
			     off_t offset, int origin)
{
	struct blkdev_info info;
	off64_t pos;
	off64_t size;
	int error;

	error = blkdev_get_info(inode->i_rdev, &info);
	if (error)
		return error;

	size = (off64_t)info.sector_size * info.nr;
	switch (origin) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = file->f_pos + offset;
		break;
	case SEEK_END:
		pos = size + offset;
		break;
	default:
		return -EINVAL;
	}

	if (pos < 0 || pos > size)
		return -EINVAL;

	file->f_pos = pos;
	return pos;
}

static int blkdev_file_read(struct inode *inode, struct file *file,
			    char *buf, int count)
{
	struct blkdev_info info;
	off64_t size;
	off64_t remaining;
	char sector[512];
	int total = 0;
	int error;

	error = blkdev_get_info(inode->i_rdev, &info);
	if (error)
		return error;
	if (info.sector_size != 512)
		return -EINVAL;
	if (file->f_pos % info.sector_size || count % info.sector_size)
		return -EINVAL;

	size = (off64_t)info.sector_size * info.nr;
	if (file->f_pos >= size)
		return 0;

	remaining = size - file->f_pos;
	if (count > remaining)
		count = remaining;

	while (count > 0) {
		error = blkdev_read(inode->i_rdev,
				    file->f_pos / info.sector_size,
				    1, sector);
		if (error)
			return error;

		memcpy_tofs(buf, sector, info.sector_size);
		buf += info.sector_size;
		file->f_pos += info.sector_size;
		count -= info.sector_size;
		total += info.sector_size;
	}

	return total;
}

static int blkdev_file_write(struct inode *inode, struct file *file,
			     char *buf, int count)
{
	struct blkdev_info info;
	off64_t size;
	char sector[512];
	int total = 0;
	int error;

	error = blkdev_get_info(inode->i_rdev, &info);
	if (error)
		return error;
	if (info.sector_size != 512)
		return -EINVAL;
	if (file->f_pos % info.sector_size || count % info.sector_size)
		return -EINVAL;

	size = (off64_t)info.sector_size * info.nr;
	if (file->f_pos + count > size)
		return -ENOSPC;

	while (count > 0) {
		memcpy_fromfs(sector, buf, info.sector_size);
		error = blkdev_write(inode->i_rdev,
				     file->f_pos / info.sector_size,
				     1, sector);
		if (error)
			return error;

		buf += info.sector_size;
		file->f_pos += info.sector_size;
		count -= info.sector_size;
		total += info.sector_size;
	}

	return total;
}

static int blkdev_file_ioctl(struct inode *inode, struct file *file,
			     unsigned int cmd, unsigned long arg)
{
	struct blkdev_info info;
	uint64_t size64;
	unsigned long size;
	int sector_size;
	int error;

	error = blkdev_get_info(inode->i_rdev, &info);
	if (error)
		return error;

	switch (cmd) {
	case BLKSSZGET:
		sector_size = info.sector_size;
		memcpy_tofs((void *)arg, &sector_size, sizeof(sector_size));
		return 0;
	case BLKGETSIZE:
		size = info.nr;
		memcpy_tofs((void *)arg, &size, sizeof(size));
		return 0;
	case BLKGETSIZE64:
		size64 = (uint64_t)info.sector_size * info.nr;
		memcpy_tofs((void *)arg, &size64, sizeof(size64));
		return 0;
	default:
		return -EINVAL;
	}
}

struct file_operations blkdev_fops = {
	.open = blkdev_file_open,
	.lseek = blkdev_file_lseek,
	.read = blkdev_file_read,
	.write = blkdev_file_write,
	.ioctl = blkdev_file_ioctl,
};
