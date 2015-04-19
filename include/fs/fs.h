#ifndef _FS_H
#define _FS_H

#include <types.h>

#include <fs/device.h>
#include <fs/blkdev.h>
#include <fs/buffer.h>

#include <fs/mariofs/mariofs.h>

struct file_system_type {
	struct super_block *(*read_super)(struct super_block *sb, void *data);
	struct list_head list;
	const char *name;
};

extern int register_file_system(struct file_system_type *fs);
extern int unregister_file_system(struct file_system_type *fs);

struct inode;
struct super_block;
struct super_operations {
	void (*read_inode)(struct inode *);
	void (*write_inode)(struct inode *);
	void (*put_inode)(struct inode *);
	void (*put_super)(struct super_block *);
	void (*write_super)(struct super_block *);
};

struct super_block {
	dev_t s_dev;
	unsigned long s_flags;
	unsigned long s_block_size;
	unsigned long s_sector_per_block;
	struct super_operations *s_ops;
	struct inode *s_covered;
	struct inode *s_mounted;
	union {
		struct mario_sb_info mario_sb;
	} u;
};

extern struct super_block *read_super(struct super_block *sb, void *data);

struct inode_operations {
	int (*create)(struct inode *,const char *,int,int,struct inode **);
};

struct inode {
	dev_t i_dev;
	unsigned long i_ino;
	dev_t i_rdev;

	off_t i_size;
	struct super_block *i_sb;
	struct inode_operations *i_ops;
	struct inode *i_mount;
	union {
		struct mario_inode_info mario_i;
	} u;
};

struct file;
struct file_operations {
	int (*lseek) (struct inode *, struct file *, off_t, int);
};

struct file {
	mode_t f_mode;
	loff_t f_pos;
	struct inode *f_inode;
	struct file_operations *f_ops;
};

#endif /* _FS_H */