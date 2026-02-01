#ifndef _FS_H
#define _FS_H

#include <types.h>
#include <time.h>
#include <misc.h>
#include <errno.h>
#include <semaphore.h>
#include <limits.h>
#include <sched.h>

#include <mm/page_alloc.h>
#include <mm/uaccess.h>
#include <mm/mm.h>

#include <lib/spinlock.h>
#include <lib/stddef.h>
#include <lib/atomic.h>
#include <lib/bitops.h>
#include <lib/string.h>
#include <lib/list.h>

#include <fs/device.h>
#include <fs/blkdev.h>
#include <fs/buffer.h>
#include <fs/chrdev.h>
#include <fs/stat.h>
#include <fs/fcntl.h>
#include <fs/pipe.h>
#include <fs/mariofs/mariofs.h>

struct file_system_type {
	struct super_block *(*read_super)(struct super_block *);
	struct list_head list;
	char *name;
};

extern struct list_head file_systems;

int register_file_system(struct file_system_type *);
int unregister_file_system(struct file_system_type *);
struct file_system_type *get_fs_type(char *);

struct inode;
struct super_block;
struct super_operations {
	int (*read_inode)(struct inode *);
	int (*write_inode)(struct inode *);
	int (*write_super)(struct super_block *);
};

struct super_block {
	dev_t s_dev;
	unsigned long s_block_size;
	unsigned long s_state;
	struct super_operations *s_op;
	struct inode *s_covered; // the inode covering this filesystem tree
	struct inode *s_mounted; // the / inode
	struct semaphore s_sem;
	unsigned long s_magic;
	union {
		struct mario_sb_info mario_sb;
	} u;
};

/* sb state bits */
#define SB_Dirty	0

#define sb_dirty(sb)	test_bit(SB_Dirty, &(sb)->s_state)

void sb_init(void);
void mount_root(void);

struct inode_operations {
	int (*lookup)(struct inode *, char *, int, struct inode **);
	int (*create)(struct inode *, char *, int, struct inode **);
	int (*truncate)(struct inode *, int);
	int (*rmdir)(struct inode *, char *, int);
	int (*mkdir)(struct inode *, char *, int);
	int (*link)(struct inode *, struct inode *, char *, int);
	int (*unlink)(struct inode *, char *, int);
	int (*rename)(struct inode *, char *, int, struct inode *, char *, int);
	int (*mknod)(struct inode *,char *,int,int,int);
};

struct file_operations;

/*
 * To simplify things, when we access/modify the content of an inode,
 * we need to down i_sem
 */
struct inode {
	dev_t i_dev;
	unsigned long i_rdev;	/* Used in mariofs to store block number or
							   device number (for files under /dev/) */
	unsigned long i_ino;
	struct super_block *i_sb;
	off_t i_size;
	unsigned long i_block_size;
	unsigned long i_nr_block;
	struct inode_operations *i_op;
	struct file_operations *i_fop;
	struct inode *i_mount; // a weak reference
	struct list_head i_list;
	struct semaphore i_sem;
	unsigned long i_state;
	unsigned long i_count;
	umode_t i_mode;
	nlink_t i_nlink;	/* Currently I don't care about this field */
	union {
		struct mario_inode_info mario_i;
		struct pipe_inode_info pipe_i;
	} u;
};

/* inode state bits */
#define I_Dirty		0
#define I_Up2date	1
#define I_Pipe		2

#define inode_dirty(i)		test_bit(I_Dirty, &(i)->i_state)
#define inode_up2date(i)	test_bit(I_Up2date, &(i)->i_state)
#define inode_pipe(i)		test_bit(I_Pipe, &(i)->i_state)

void inode_init(void);
struct inode *get_empty_inode(void);
struct inode *get_pipe_inode(void);
struct inode *iget(struct super_block *, int);
void iput(struct inode *);
void iref(struct inode *);

typedef int (*filldir_t)(void *, char *, int, off_t, ino_t);

struct file;
struct file_operations {
	int (*open)(struct inode *, struct file *);
	void (*release)(struct inode *, struct file *);
	int (*lseek)(struct inode *, struct file *, off_t, int);
	int (*read)(struct inode *, struct file *, char *, int);
	int (*write)(struct inode *, struct file *, char *, int);
	int (*readdir)(struct inode *, struct file *, void *, filldir_t);
	int (*mmap)(struct inode *, struct file *, struct vm_area_struct *);
	int (*ioctl)(struct inode *, struct file *, unsigned int, unsigned long);
};

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

struct file {
	loff_t f_pos;
	mode_t f_mode;
	unsigned short f_flags;
	unsigned short f_count;
	struct inode *f_inode;
	struct file_operations *f_op;
	void *private_data;
};

void file_init(void);
struct file *get_empty_file(void);
int get_two_empty_files(struct file *res[2]);
void put_file(struct file *);
void fref(struct file *);

struct fs_struct {
	int count;
	struct inode *root, *pwd;
};

struct files_struct {
	int count;
	fd_set close_on_exec;
	struct file *fd[NR_OPEN];
};

void fs_init(void);

int getname(const char *, char **);
void putname(char *);

int namei(const char *, struct inode **);
int open_namei(char *, int, struct inode **,struct inode *);

int generic_file_mmap(struct inode *, struct file *, struct vm_area_struct *);

#endif /* _FS_H */