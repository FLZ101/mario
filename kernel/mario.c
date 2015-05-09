#include <multiboot.h>
#include <unistd.h>
#include <sched.h>
#include <trap.h>
#include <time.h>
#include <timer.h>
#include <misc.h>
#include <irq.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>
#include <mm/paging.h>
#include <mm/e820.h>

#include <lib/stdarg.h>

#include <fs/fs.h>

void printf(const char *fmt, ...);

void kernel_thread(void (*fun)(void *), void *arg)
{
	__asm__ __volatile__(
		"movl %%esp, %%esi\n\t"
		"movl $1, %%eax\n\t"
		"int $0x80\n\t"
		"cmpl %%esp, %%esi\n\t"
		"je 1f\n\t"
		"pushl %0\n\t"
		"call *%1\n\t"
		"jmp .\n\t"
		"1:"
		:
		:"b"(arg), "c"(fun)
		:"eax", "esi", "memory");
}

void page_alloc_print(void)
{
	int i;
	for (i = 3; i >= 0; i--)
		free_list_print(i);
}

void test_mm(void)
{
	/* test page_malloc */
	unsigned long x;
	page_alloc_print();
	x = page_alloc();
	early_print("%x\n", x);
	page_free(x);
	page_alloc_print();
	x = page_alloc();
	early_print("%x\n", x);
	page_free(x);

	/* test kmalloc */
	void *p;
	p = kmalloc(123);
	early_print("%x\n", (unsigned int)p);
	kfree(p);
	p = kmalloc(123);
	early_print("%x\n", (unsigned int)p);
	kfree(p);
}

void test_timer(unsigned long data)
{
	early_print("\t%x\n", data);

	struct timer_list *timer = kmalloc(sizeof(*timer));
	init_timer(timer);
	timer->expires = 3*HZ;
	timer->fun = test_timer;
	timer->data = (unsigned long)timer;
	add_timer(timer);
}

int sys_alarm(long seconds);

void timer_thread(unsigned int n)
{
	struct itimerval it_new;
	it_new.it_interval.tv_usec = 0;
	it_new.it_value.tv_usec = 0;

	it_new.it_interval.tv_sec = 1;
	it_new.it_value.tv_sec = 1;
	_setitimer(ITIMER_REAL, &it_new, NULL);

	it_new.it_interval.tv_sec = 1;
	it_new.it_value.tv_sec = 1;
	_setitimer(ITIMER_VIRTUAL, &it_new, NULL);

	it_new.it_interval.tv_sec = 1;
	it_new.it_value.tv_sec = 1;
	_setitimer(ITIMER_PROF, &it_new, NULL);

	/*
	sys_alarm(7);
	*/

	test_timer(n);
}

void test_blkdev(void *arg)
{
	int n;
	struct buffer_head *bh;
	char *buf;

	n = (int)arg;
	bh = bread(MKDEV(RD_MAJOR, 0), 0);
	buf = bh->b_data;
	buf[10] = '\0';
	printf("%s%u\n", buf, n);
	set_dirty(bh);
	brelse(bh);
}

void init(void *arg)
{
	int n = (int)arg;

	struct buffer_head *bh = get_buffer(MKDEV(RD_MAJOR, 0), 0);
	printf("blkdev test starts\n");
	while (n--)
		kernel_thread(test_blkdev, (void *)n);

	schedule_timeout(3*HZ);
	brelse(bh);
}

void cpu_idle(void)
{
	for (; ; )
		schedule();
}

extern int sys_open(const char *filename, int flags);
extern int sys_read(unsigned int fd, char *buf, unsigned int count);
extern int sys_write(unsigned int fd, char *buf, unsigned int count);
extern int sys_close(unsigned int fd);
extern int sys_lseek(unsigned int fd, off_t offset, unsigned int origin);
extern int sys_stat(char *filename, struct stat *statbuf);
extern int sys_fstat(unsigned int fd, struct stat *statbuf);
extern int sys_truncate(const char *path, int length);
extern int sys_creat(const char *pathname);
extern int sys_ftruncate(unsigned int fd, int length);
extern int sys_chdir(const char *filename);
extern int sys_rmdir(char *pathname);
extern int sys_mkdir(char *pathname);
extern int sys_chroot(char *filename);
extern int sys_fchdir(char *filename);
extern int sys_link(char *oldname, char *newname);
extern int sys_unlink(char *pathname);
extern int sys_rename(char *oldname, char *newname);
extern int sys_getdents(unsigned int fd, void *dirent, unsigned int count);

char *write =
"IT WAS in the year '95 that a combination of events, into which I need not "
"enter, caused Mr. Sherlock Holmes and myself to spend some weeks in "
"one of our great university towns, and it was during this time that the "
"small but instructive adventure which I am about to relate befell us. It will "
"be obvious that any details which would help the reader exactly to "
"identify the college or the criminal would be injudicious and offensive. So "
"painful a scandal may well be allowed to die out. With due discretion the "
"incident itself may, however, be described, since it serves to illustrate "
"some of those qualities for which my friend was remarkable. I will "
"endeavour, in my statement, to avoid such terms as would serve to limit "
"the events to any particular place, or give a clue as to the people "
"concerned. ";

void print_stat(struct stat *st)
{
	char *mode[] = {"REG", "DIR", "BLK", "CHR"};

	early_print("dev=%x, ", st->st_dev);
	early_print("ino=%x, ", st->st_ino);
	early_print("mode=%s, ", mode[st->st_mode]);
	early_print("rdev=%u\n", st->st_rdev);
	early_print("size=%d, ", st->st_size);
	early_print("blksz=%d, ", st->st_blksize);
	early_print("blks=%x\n", st->st_blocks);
}

void print_inode(struct inode *i)
{
	char *mode[] = {"REG", "DIR", "BLK", "CHR"};

	early_print("dev=%x, ", i->i_dev);
	early_print("ino=%x, ", i->i_ino);
	early_print("mode=%s, ", mode[i->i_mode]);
	early_print("rdev=%u\n", i->i_rdev);
	early_print("size=%d, ", i->i_size);
	early_print("blksz=%d, ", i->i_block_size);
	early_print("blks=%x\n", i->i_nr_block);
}

struct mario_dirent {
	unsigned long d_ino;
	unsigned long d_off;
	unsigned short d_reclen;
	char d_name[1];
};

/* list content of a directory */
void ls(char *dirname)
{
	int fd, count, pos;
	char buf[100];
	struct mario_dirent *de;

	fd = sys_open(dirname, O_RDONLY);
	if (fd < 0) {
		early_print("TROUBLE0 :(\n");
		return;
	}
try:
	count = sys_getdents(fd, buf, 100);
	if (count <= 0) {
		sys_close(fd);
		return;
	}
	pos = 0;
	while (pos < count) {
		de = (struct mario_dirent *)(buf + pos);
		early_print("%d, %s\n", de->d_ino, de->d_name);
		pos += de->d_reclen;
	}
	goto try;
}

void test_fs(void)
{
#if 0
	int fd;
	struct stat st;
	char buf[1200] = {0, };

	fd = sys_creat("/dev/her.txt");
	early_print("write = %d\n", sys_write(fd, write, strlen(write)));
	print_inode(current->files->fd[fd]->f_inode);
	sys_close(fd);
	fd = sys_open("/dev/her.txt", O_RDWR);
	early_print("write = %d\n", sys_write(fd, write, 10));
	print_inode(current->files->fd[fd]->f_inode);
	sys_ftruncate(fd, 0);
	print_inode(current->files->fd[fd]->f_inode);
	sys_lseek(fd, 0, 0);
	early_print("read = %d\n", sys_read(fd, buf, 1200));
	early_print("%s\n", buf);
	//sys_fstat(fd, &st);
	//print_stat(&st);
	sys_close(fd);

	sys_stat("/dev/her.txt", &st);
	print_stat(&st);
#endif
#if 0
	//ls("./apple");
	//sys_chdir("dev");
	//ls("../.././dev");
	ls("tmp");
	sys_rmdir("/tmp/fuck");
	ls("tmp");
	/*
	sys_mkdir("/tmp/her");
	ls("/tmp");
	*/
	sys_chroot("tmp");
	sys_mkdir("/her");
	sys_chdir("/her");
	sys_chdir(".");
	ls(".");
	sys_chdir("..");
	ls(".");
#endif
#if 0
	//early_print("%d\n", sys_link("99.txt", "999.txt"));
	int fd;
	ls("/tmp");
	fd = sys_creat("/tmp/0.txt");
	sys_close(fd);
	ls("tmp");
	sys_unlink("tmp/0.txt");
	ls("tmp");
	fd = sys_creat("/tmp/1.txt");
	sys_close(fd);
	ls("tmp");
#endif
	ls("apple");
	ls("tmp");
	sys_rename("apple/38.txt", "apple/83.fuck");
	sys_rename("apple/pear", "tmp/pear");
	ls("apple");
	sys_chdir("tmp/pear/..");
	ls(".");
}

void bh_thread(void *arg);

int _namei(char *pathname, struct inode *base, struct inode **res_inode);
void mario(struct multiboot_info *m)
{
	early_print_init(m);
	setup_memory_region(m);
	paging_init();
	page_alloc_init();
	trap_init();
	irq_init();
	time_init();

	blkdev_init();
	buffer_init();
	fs_init();
	test_fs();
	sti();

	/*
	kernel_thread(init, (void *)10000);
	kernel_thread(bh_thread, NULL);
	*/

	cpu_idle();
}
