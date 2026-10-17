#ifndef _FS_PROC_H
#define _FS_PROC_H

struct inode;
struct inode_operations;

extern struct inode_operations proc_dir_iops;

struct inode *proc_get_root(void);

#endif /* _FS_PROC_H */
