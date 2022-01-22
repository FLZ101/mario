#include <fs/fs.h>
#include <fs/pe.h>

#include <trap.h>
#include <errno.h>
#include <signal.h>

#include <mm/page_alloc.h>
#include <mm/uaccess.h>
#include <mm/mman.h>
#include <mm/mm.h>

#define PE_IMAGE_BASE	0x00400000

#define PE_SEC_NR	96

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES	32

struct exec {
	char *comm;
	struct pe_sec_hdr sec[3];	/* .text, .data, .bss */
	unsigned long image_base, entry;
	int argc, envc;
	unsigned long arg_page[MAX_ARG_PAGES], arg_p;
};

extern int do_open(char *filename, int flags);

int open_exec(char *filename)
{
	int fd;
	struct file *file;
	struct inode *inode;

	fd = do_open(filename, O_RDONLY);
	if (fd < 0)
		return fd;
	file = current->files->fd[fd];
	inode = file->f_inode;
	if (!inode || !S_ISREG(inode->i_mode) || !inode->i_sb)
		return -EACCES;
	if (!file->f_op || !file->f_op->read)
		return -EACCES;
	return fd;
}

int check_coff_header(struct pe_coff_hdr *coff_hdr)
{
	if (coff_hdr->machine_type != PE_MACHINE_I386)
		return 1;
	if (!(coff_hdr->coff_flags & PE_RELOCS_STRIPPED))
		return 1;
	if (!(coff_hdr->coff_flags & PE_EXECUTABLE_IMAGE))
		return 1;
	if (!(coff_hdr->coff_flags & PE_32BIT_MACHINE))
		return 1;
	if (coff_hdr->num_of_sec > PE_SEC_NR)
		return 1;
	return 0;
}

int check_opt_header(struct pe_opt_hdr *opt_hdr)
{
	if (opt_hdr->magic != PE32_MAGIC)
		return 1;
	if (opt_hdr->image_base != PE_IMAGE_BASE)
		return 1;
	if (opt_hdr->sec_align != opt_hdr->file_align)
		return 1;
	if (opt_hdr->sec_align != 0x1000)
		return 1;
	return 0;
}

void fill_exec(struct pe_sec_hdr *sec_hdr_tbl, int nr, struct exec *exe)
{
	int i;

	for (i = 0; i < 3; i++)
		exe->sec[i].sec_flags = 0;
	for (i = 0; i < nr; i++) {
		if (!strcmp(".text", sec_hdr_tbl[i].name)) {
			exe->sec[0] = sec_hdr_tbl[i];
		} else if (!strcmp(".data", sec_hdr_tbl[i].name)) {
			exe->sec[1] = sec_hdr_tbl[i];
		} else if (!strcmp(".bss", sec_hdr_tbl[i].name)) {
			exe->sec[2] = sec_hdr_tbl[i];
		}
	}
}

int check_exec(struct exec *exe, struct pe_opt_hdr *opt_hdr)
{
	unsigned long flags;
	struct pe_sec_hdr *a, *b;

	/* No .text section found */
	if (!exe->sec[0].sec_flags)
		return 1;
	/*
	 * check whether these sections are contiguous in memory
	 */
	for (a = exe->sec, b = a + 1; b < exe->sec + 3; a = b++) {
		/* empty section? */
		if (!b->sec_flags) {
			b->vir_addr = a->vir_addr + a->vir_sz;
			b->vir_sz = 0;
			continue;
		}
		if (b->vir_addr != a->vir_addr + a->vir_sz)
			return 1;
	}

	flags = exe->sec[0].sec_flags;
	if (!(flags & PE_SEC_TEXT) || !(flags & PE_SEC_EXEC))
		return 1;
	if (exe->sec[0].vir_addr != opt_hdr->text_base)
		return 1;
	if (exe->sec[0].vir_sz != opt_hdr->text_sz)
		return 1;

	if (!(flags = exe->sec[1].sec_flags))
		goto __next;
	if (!(flags & PE_SEC_DATA) || !(flags & PE_SEC_WRITE))
		return 1;
	if (exe->sec[1].vir_sz != opt_hdr->data_sz)
		return 1;
__next:
	if (!(flags = exe->sec[2].sec_flags))
		return 0;
	if (!(flags & PE_SEC_BSS) || !(flags & PE_SEC_WRITE))
		return 1;
#if 0
	if (exe->sec[2].vir_sz != opt_hdr->bss_sz)
		return 1;
#endif
	return 0;
}

static inline int exec_read(struct file *file, void *buf, unsigned int count)
{
	return count != file->f_op->read(file->f_inode, file, buf, count);
}

int prep_exec(struct file *file, struct exec *exe)
{
	int i, error = 0;
	__u32 tmp;
	struct pe_coff_hdr coff_hdr;
	struct pe_opt_hdr opt_hdr;
	struct pe_sec_hdr *sec_hdr_tbl;

	/*
	 * read offset of PE signature
	 */
	file->f_pos = 0x3c;
	if (exec_read(file, &tmp, 4))
		return -EIO;
	/*
	 * read and check PE signature
	 */
	file->f_pos = tmp;
	if (exec_read(file, &tmp, 4))
		return -EIO;
	if (tmp != PE_SIGNATURE)
		return -EINVAL;
	/*
	 * read and check PE coff header
	 */
	if (exec_read(file, &coff_hdr, sizeof(coff_hdr)))
		return -EIO;
	if (check_coff_header(&coff_hdr))
		return -EINVAL;
	/*
	 * read and check PE optional header
	 */
	if (exec_read(file, &opt_hdr, sizeof(opt_hdr)))
		return -EIO;
	if (check_opt_header(&opt_hdr))
		return -EINVAL;
	exe->image_base = opt_hdr.image_base;
	exe->entry = opt_hdr.entry;
	/*
	 * skip optional header data directories
	 */
	tmp = opt_hdr.num_of_data_dir_ent * PE_DATA_DIR_ENT_SZ;
	if (tmp + sizeof(opt_hdr) != coff_hdr.opt_hdr_sz)
		return -EINVAL;
	file->f_pos += tmp;

	/* size of section header table */
	tmp = sizeof(struct pe_sec_hdr) * coff_hdr.num_of_sec;
	sec_hdr_tbl = (struct pe_sec_hdr *)page_alloc();
	if (!sec_hdr_tbl)
		return -ENOMEM;
	/* read section header table */
	if (exec_read(file, sec_hdr_tbl, tmp)) {
		error = -EIO;
		goto tail;
	}
	fill_exec(sec_hdr_tbl, coff_hdr.num_of_sec, exe);
	if (check_exec(exe, &opt_hdr))
		error = -EINVAL;

	for (i = 0; i < 3; i++)
		exe->sec[i].vir_addr += exe->image_base;
	exe->entry += exe->image_base;
tail:
	page_free((unsigned long)sec_hdr_tbl);
	return error;
}

/*
 * Hope this works
 */
int copy_one_arg(struct exec *exe, char *p, int *n)
{
	char *tmp;
	int len, error = 0;
	unsigned long a, b, c, d;

	error = getname(p, &tmp);
	if (error) {
		if (error != -ENOMEM)
			error = -EFAULT;
		return error;
	}
	len = strlen(tmp) + 1;
	if (len == 1)	/* empty string? */
		goto tail;
	a = exe->arg_p & ~PAGE_MASK;
	b = (a + PAGE_SIZE) & PAGE_MASK;
	c = exe->arg_p / PAGE_SIZE;
	d = exe->arg_page[c] + a;
	if (a + len >= b) {
		len -= b - a;
		memcpy((void *)d, tmp, b - a);
		exe->arg_p += b - a;
		tmp += b - a;
		if (c == MAX_ARG_PAGES - 1) {
			error = -E2BIG;
			goto tail;
		}
		if (!(exe->arg_page[++c] = get_zero_page())) {
			error = -ENOMEM;
			goto tail;
		}
		d = exe->arg_page[c];
	}
	if (len) {
		memcpy((void *)d, tmp, len);
		exe->arg_p += len;
	}
	(*n)++;
tail:
	putname(tmp);
	return error;
}

int copy_args(struct exec *exe, char **arg, int *n)
{
	int error;
	char *p;

	if (!arg)
		return 0;
	for (; ; arg++) {
		error = verify_area(VERIFY_READ, arg, sizeof(void *));
		if (error)
			return error;
		p = (char *)get_fs_long(arg);
		if (!p)
			break;
		error = copy_one_arg(exe, p, n);
		if (error)
			return error;
	}
	return 0;
}

int prep_args(struct exec *exe, char **argv, char **envp)
{
	int i, error;

	if (!(exe->arg_page[0] = page_alloc()))
		return -ENOMEM;
	for (i = 1; i < MAX_ARG_PAGES; i++)
		exe->arg_page[i] = 0;
	exe->arg_p = 0;
	exe->argc = 0;
	exe->envc = 0;

	if ((error = copy_args(exe, argv, &exe->argc)))
		return error;
	if ((error = copy_args(exe, envp, &exe->envc)))
		return error;
	return 0;
}

extern void exit_mmap(struct task_struct *);
extern int sys_close(unsigned int);
extern void clear_page_tables(struct task_struct *);

void flush_old_exec(struct exec *exe)
{
	int i;
	char ch, *name;

	for (i = 0, name = exe->comm; (ch = *(name++)) != '\0'; ) {
		if (ch == '/')
			i = 0;
		else
			if (i < 15)
				current->comm[i++] = ch;
	}
	current->comm[i] = '\0';

	exit_mmap(current);
	current->mm->mmap = NULL;

	current->signal = 0;
	for (i = 0; i < 32; i++) {
		current->sigaction[i].sa_mask = 0;
		current->sigaction[i].sa_flags = 0;
		if (current->sigaction[i].sa_handler != SIG_IGN)
			current->sigaction[i].sa_handler = NULL;
	}
	for (i = 0; i < NR_OPEN; i++)
		if (FD_ISSET(i, &current->files->close_on_exec))
			sys_close(i);
	FD_ZERO(&current->files->close_on_exec);
	clear_page_tables(current);
}

extern unsigned long do_mmap(unsigned long addr, unsigned long len,
	unsigned long prot, unsigned long flags, int fd, unsigned long off);

extern int set_page(struct mm_struct *mm,
	unsigned long page, unsigned long addr);

int setup_arg_pages(struct exec *exe)
{
	int i;
	char *pc;
	unsigned long tmp, nr_page, start_stack, stack_page, *p1, *p2;

	nr_page = exe->arg_p / PAGE_SIZE + 1;
	start_stack = KERNEL_BASE - nr_page * PAGE_SIZE;
	for (i = 0; i < nr_page; i++)
		if (set_page(current->mm, exe->arg_page[i], start_stack + i * PAGE_SIZE)) {
			goto tail_1;
		} else {
			exe->arg_page[i] = 0;
		}

	current->mm->start_stack = start_stack;

	stack_page = get_zero_page();
	if (!stack_page)
		return 1;
	if (set_page(current->mm, stack_page, start_stack - PAGE_SIZE)) {
		put_page(stack_page);
		return 1;
	}

	/*
	 * `push' envp[], argv[], argc
	 */
	tmp = exe->argc + exe->envc + 5;	/* words we should push */
	if (tmp >= 1024)	/* kind of ugly */
		return 1;

	p1 = (unsigned long *)start_stack - tmp;
	/* `push' argc */
	*(p1) = exe->argc;

	pc = (char *)start_stack;
	current->mm->arg_start = (unsigned long)pc;

	/* `push' argv[] */
	p2 = p1 + 3;
	*(p1 + 1) = (unsigned long)p2;
	for (i = 0; i < exe->argc; i++) {
		*(p2++) = (unsigned long)pc;
		while (*(pc++))
			;
	}
	*(p2++) = 0;
	current->mm->arg_end = current->mm->env_start = (unsigned long)pc;

	/* `push' envp[] */
	*(p1 + 2) = (unsigned long)p2;
	for (i = 0; i < exe->envc; i++) {
		*(p2++) = (unsigned long)pc;
		while (*(pc++))
			;
	}
	*(p2++) = 0;
	current->mm->env_end = (unsigned long)pc;

	/* mmap stack */
	do_mmap(start_stack - PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | VM_GROWSDOWN, -1, 0);
	return 0;

tail_1:
	for (i = 0; i < MAX_ARG_PAGES; i++)
		if (exe->arg_page[i])
			put_page(exe->arg_page[i]);
	return 1;
}

static inline void start_thread(struct trap_frame *tr,
	unsigned long eip, unsigned long esp)
{
	__asm__("movl %0,%%fs ; movl %0,%%gs": :"r" (0));
	set_fs(USER_DS);
	tr->cs = USER_CS;
	tr->ds = tr->es = tr->ss = USER_DS;
	tr->eip = eip;
	tr->esp = esp;
}

int do_exec(struct exec *exe, int fd, struct trap_frame *tr)
{
	/* no turning back */
	flush_old_exec(exe);

	current->mm->start_code = exe->sec[0].vir_addr;
	current->mm->end_code = current->mm->start_data = exe->sec[1].vir_addr;
	current->mm->end_data = current->mm->start_brk = exe->sec[2].vir_addr;
	current->mm->brk = current->mm->start_brk + exe->sec[2].vir_sz;

	/* mmap .text section */
	do_mmap(exe->sec[0].vir_addr, exe->sec[0].vir_sz,
		PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE,
			fd, exe->sec[0].raw_off);
	/* mmap .data section */
	do_mmap(exe->sec[1].vir_addr, exe->sec[1].vir_sz,
		PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE,
			fd, exe->sec[1].raw_off);
	/* mmap .bss section */
	do_mmap(exe->sec[2].vir_addr, exe->sec[2].vir_sz, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
			-1, 0);
	setup_arg_pages(exe);

	current->did_exec = 1;

	start_thread(tr, exe->entry,
		current->mm->start_stack - 4 * (exe->argc + exe->envc + 5));
	return 0;
}

int do_execve(char *filename, char **argv, char **envp, struct trap_frame *tr)
{
	int i, fd, error;
	struct file *file;
	struct exec exe;

	fd = open_exec(filename);
	if (fd < 0)
		return fd;
	file = current->files->fd[fd];

	exe.comm = filename;
	error = prep_exec(file, &exe);
	if (error)
		goto tail_1;
	error = prep_args(&exe, argv, envp);
	if (error)
		goto tail_2;
	error = do_exec(&exe, fd, tr);
	if (!error)
		return 0;
tail_2:
	for (i = 0; i < MAX_ARG_PAGES && exe.arg_page[i]; i++)
		page_free(exe.arg_page[i]);
tail_1:
	sys_close(fd);
	return error;
}

int sys_execve(struct trap_frame tr)
{
	int error;
	char *filename;

	error = getname((char *)tr.ebx, &filename);
	if (error)
		return error;
	error = do_execve(filename, (char **)tr.ecx, (char **)tr.edx, &tr);
	putname(filename);
	return error;
}
