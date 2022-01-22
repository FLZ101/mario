#include <errno.h>

#include <lib/string.h>

#include <fs/fs.h>

struct list_head file_systems = LIST_HEAD_INIT(file_systems);

struct file_system_type *get_fs_type(char *name)
{
	struct list_head *pos;
	struct file_system_type *tmp;

	list_for_each(pos, &file_systems) {
		tmp = list_entry(pos, struct file_system_type, list);
		if (!strcmp(name, tmp->name))
			return tmp;
	}
	return NULL;
}

int register_file_system(struct file_system_type *fs)
{
	if (!fs)
		return -EINVAL;

	if (get_fs_type(fs->name))
		return -EBUSY;

	list_add_tail(&fs->list, &file_systems);
	return 0;
}

int unregister_file_system(struct file_system_type *fs)
{
	struct list_head *pos;
	struct file_system_type *tmp;

	list_for_each(pos, &file_systems) {
		tmp = list_entry(pos, struct file_system_type, list);
		if (tmp == fs) {
			list_del(&fs->list);
			return 0;
		}
	}
	return -EINVAL;
}
