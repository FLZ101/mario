#include <fs/fs.h>
#include <misc.h>

void __tinit fs_init(void)
{
	sb_init();
	inode_init();
	file_init();
	register_file_system(&mariofs);
	mount_root();
}