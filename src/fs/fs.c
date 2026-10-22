#include <fs/fs.h>
#include <fs/proc.h>
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
	mount_root();
}
