#include <fs/fs.h>

#include <misc.h>

#include <sched.h>

static struct list_head lru;
static spinlock_t inode_lock;
static wait_queue_t inode_wait;

#define NR_INODE	2048

static struct inode all[NR_INODE];

void __tinit inode_init(void)
{
	int i;

	INIT_LIST_HEAD(&lru);
	INIT_LOCK(&inode_lock);
	init_wait_queue(&inode_wait);

	for (i = 0; i < NR_INODE; i++) {
		memset(all + i, 0, sizeof(struct inode));
		init_MUTEX(&all[i].i_sem);
		list_add_tail(&all[i].i_list, &lru);
	}
}

static void read_inode(struct inode *i)
{
	if (inode_up2date(i))
		return;
	if (i->i_sb && i->i_sb->s_op && i->i_sb->s_op->read_inode)
		i->i_sb->s_op->read_inode(i);

	set_bit(I_Up2date, &i->i_state);
	clear_bit(I_Dirty, &i->i_state);
}

static void write_inode(struct inode *i)
{
	if (!inode_up2date(i) || !inode_dirty(i))
		return;

	if (i->i_sb && i->i_sb->s_op && i->i_sb->s_op->write_inode)
		i->i_sb->s_op->write_inode(i);
	clear_bit(I_Dirty, &i->i_state);
}

static void clear_inode(struct inode *i)
{
	struct list_head i_list = i->i_list;
	struct semaphore i_sem = i->i_sem;

	memset(i, 0, sizeof(struct inode));

	i->i_list = i_list;
	i->i_sem = i_sem;
}

struct inode *get_empty_inode(void)
{
	struct inode *i;
try:
	ACQUIRE_LOCK(&inode_lock);
	i = list_entry(lru.next, struct inode, i_list);
	if (i->i_count) {
		sleep_on(&inode_wait, TASK_UNINTERRUPTIBLE, &inode_lock);
		goto try;
	}
	i->i_count = 1;
	list_del(&i->i_list);
	list_add_tail(&i->i_list, &lru);
	RELEASE_LOCK(&inode_lock);
	return i;
}

struct inode *get_pipe_inode(void)
{
	struct inode *i;

	if (!(i = get_empty_inode()))
		return NULL;

	set_bit(I_Pipe, &i->i_state);
	i->i_mode = MODE_FIFO;
	i->i_block_size = PAGE_SIZE;
	return i;
}

static struct inode *find_inode(struct super_block *sb, int ino)
{
	struct list_head *pos;
	struct inode *tmp;

	list_for_each(pos, &lru) {
		tmp = list_entry(pos, struct inode, i_list);
		if (tmp->i_dev == sb->s_dev && tmp->i_ino == ino)
			return tmp;
	}
	return NULL;
}

struct inode *__iget(struct super_block *sb, int ino, int crossmnt)
{
	struct inode *i;
try:
	ACQUIRE_LOCK(&inode_lock);
	if ((i = find_inode(sb, ino)))
		goto tail;
	i = list_entry(lru.next, struct inode, i_list);
	if (i->i_count) {
		sleep_on(&inode_wait, TASK_UNINTERRUPTIBLE, &inode_lock);
		goto try;
	}
	clear_inode(i);
	i->i_sb = sb;
	i->i_dev = sb->s_dev;
	i->i_ino = ino;
	i->i_mount = NULL;
tail:
	i->i_count++;
	list_del(&i->i_list);
	list_add_tail(&i->i_list, &lru);
	RELEASE_LOCK(&inode_lock);
	down(&i->i_sem);
	write_inode(i);
	read_inode(i);
	up(&i->i_sem);
	if (crossmnt && i->i_mount) {
		struct inode *tmp = i->i_mount;
		iref(tmp);
		iput(i);
		i = tmp;
	}
	return i;
}

struct inode *iget(struct super_block *sb, int ino)
{
	return __iget(sb, ino, 1);
}

/*
 * make @i the last 'free' (which means @i->i_count is 0) one in lru
 */
static void into_lru_list(struct inode *i)
{
	struct list_head *pos;
	struct inode *tmp;

	list_for_each(pos, &lru) {
		tmp = list_entry(pos, struct inode, i_list);
		if (tmp->i_count)
			break;
	}
	list_del(&i->i_list);
	list_add_tail(&i->i_list, pos);
}

void iput(struct inode *i)
{
	ACQUIRE_LOCK(&inode_lock);

	early_assert(i->i_count > 0);

	if (!--i->i_count) {
		write_inode(i);
		into_lru_list(i);

		if (inode_pipe(i)) {
			destroy_pipe_inode_info(&i->u.pipe_i);
			clear_bit(I_Pipe, &i->i_state);
		}

		wake_up_all(&inode_wait);
	}
	RELEASE_LOCK(&inode_lock);
}

/*
 * Ugly function :(
 * Just want to feel safe
 */
void iref(struct inode *i)
{
	ACQUIRE_LOCK(&inode_lock);
	i->i_count++;
	RELEASE_LOCK(&inode_lock);
}

int fs_may_mount(dev_t dev)
{
	struct list_head *pos;
	struct inode *tmp;

	list_for_each(pos, &lru) {
		tmp = list_entry(pos, struct inode, i_list);
		if (tmp->i_dev != dev)
			continue;
		if (tmp->i_count || (inode_up2date(tmp) && inode_dirty(tmp)))
			return 0;
	}
	return 1;
}

int fs_may_umount(dev_t dev, struct inode *mount_root)
{
	struct list_head *pos;
	struct inode *tmp;

	list_for_each(pos, &lru) {
		tmp = list_entry(pos, struct inode, i_list);
		if (tmp->i_dev != dev || !tmp->i_count)
			continue;
		if (tmp == mount_root && tmp->i_count == 1)
			continue;
		return 0;
	}
	return 1;
}