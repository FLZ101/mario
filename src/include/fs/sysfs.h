#ifndef _FS_SYSFS_H
#define _FS_SYSFS_H

#define SYSFS_SUPER_MAGIC	0x62656572

struct inode;
struct file_system_type;
struct inode_operations;

extern struct inode_operations sysfs_dir_iops;
extern struct file_system_type sysfs;

struct inode *sysfs_get_root(void);

#endif /* _FS_SYSFS_H */
