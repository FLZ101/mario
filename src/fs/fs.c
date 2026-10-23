#include <fs/fs.h>
#include <fs/proc.h>
#include <fs/sysfs.h>
#include <fs/debugfs.h>
#include <misc.h>

void fs_init(void)
{
	sb_init();
	inode_init();
	file_init();
	register_file_system(&mariofs);
	register_file_system(&ext2);
	register_file_system(&fat);
	register_file_system(&proc);
	register_file_system(&sysfs);
	register_file_system(&debugfs);
	pr_info("Filesystems registered\n");
	mount_root();
	pr_info("Root filesystem mounted\n");
}
