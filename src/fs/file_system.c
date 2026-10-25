#include <errno.h>

#include <lib/string.h>
#include <lib/spinlock.h>

#include <fs/fs.h>

struct list_head file_systems = LIST_HEAD_INIT(file_systems);
/*
 * file_systems is populated in fs_init() before interrupts are enabled, but
 * it is also walked by sys_mount()/read_super() at runtime.  Use an IRQ-safe
 * spinlock so registration, unregistration, and lookups cannot race on SMP.
 */
static spinlock_t file_systems_lock = SPINLOCK_UNLOCKED;

static struct file_system_type *get_fs_type_locked(char *name)
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

struct file_system_type *get_fs_type(char *name)
{
	struct file_system_type *fs;
	unsigned long flags;

	spin_lock_irqsave(&file_systems_lock, flags);
	fs = get_fs_type_locked(name);
	spin_unlock_irqrestore(&file_systems_lock, flags);
	return fs;
}

int register_file_system(struct file_system_type *fs)
{
	unsigned long flags;

	if (!fs)
		return -EINVAL;

	spin_lock_irqsave(&file_systems_lock, flags);
	if (get_fs_type_locked(fs->name)) {
		spin_unlock_irqrestore(&file_systems_lock, flags);
		return -EBUSY;
	}

	list_add_tail(&fs->list, &file_systems);
	spin_unlock_irqrestore(&file_systems_lock, flags);
	return 0;
}

int unregister_file_system(struct file_system_type *fs)
{
	struct list_head *pos;
	struct file_system_type *tmp;
	unsigned long flags;

	spin_lock_irqsave(&file_systems_lock, flags);
	list_for_each(pos, &file_systems) {
		tmp = list_entry(pos, struct file_system_type, list);
		if (tmp == fs) {
			list_del(&fs->list);
			spin_unlock_irqrestore(&file_systems_lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&file_systems_lock, flags);
	return -EINVAL;
}
