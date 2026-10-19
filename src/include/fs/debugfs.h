#ifndef _FS_DEBUGFS_H
#define _FS_DEBUGFS_H

struct inode;
struct inode_operations;

extern struct inode_operations debugfs_dir_iops;

struct inode *debugfs_get_root(void);

#endif /* _FS_DEBUGFS_H */
