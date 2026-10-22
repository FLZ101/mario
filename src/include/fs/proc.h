#ifndef _FS_PROC_H
#define _FS_PROC_H

#define PROC_SUPER_MAGIC	0x9fa0

struct inode;
struct file_system_type;
struct inode_operations;

extern struct inode_operations proc_dir_iops;
extern struct file_system_type proc;

struct inode *proc_get_root(void);

#endif /* _FS_PROC_H */
