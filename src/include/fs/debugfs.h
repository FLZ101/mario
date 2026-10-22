#ifndef _FS_DEBUGFS_H
#define _FS_DEBUGFS_H

#define DEBUGFS_SUPER_MAGIC	0x64626720

struct inode;
struct file_system_type;
struct inode_operations;

extern struct inode_operations debugfs_dir_iops;
extern struct file_system_type debugfs;

struct inode *debugfs_get_root(void);

#endif /* _FS_DEBUGFS_H */
