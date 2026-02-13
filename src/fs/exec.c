#include <fs/fs.h>
#include <fs/elf.h>

#include <trap.h>
#include <errno.h>
#include <signal.h>

#include <mm/page_alloc.h>
#include <mm/kmalloc.h>
#include <mm/uaccess.h>
#include <mm/mman.h>
#include <mm/mm.h>

/*
 * MAX_ARG_PAGES defines the number of pages allocated for arguments
 * and envelope for the new program. 32 should suffice, this gives
 * a maximum env+arg of 128kB !
 */
#define MAX_ARG_PAGES	32

#define MAX_PHDRS 10

struct exec {
	char *comm;
	struct Elf32_Phdr phdr[MAX_PHDRS];
	unsigned long entry;
	int argc, envc;
	unsigned long arg_page[MAX_ARG_PAGES], arg_p;

	unsigned long start_code, end_code, start_data, end_data, start_brk, brk;
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

int check_elf_header(struct Elf32_Ehdr *ehdr)
{
	if (!CHECK_EHDR_MAGIC(ehdr->e_ident))
		return 1;
	if (ehdr->e_ident[EI_CLASS] != ELFCLASS32)
		return 1;
	if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
		return 1;
	if (ehdr->e_type != ET_EXEC)
		return 1;
	if (ehdr->e_machine != EM_386)
		return 1;
	if (ehdr->e_entry == 0)
		return 1;
	if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0)
		return 1;
	return 0;
}

int fill_exec(struct Elf32_Phdr *phdr_tbl, int nr, struct exec *exe)
{
	int i, j;

	for (i = 0; i < MAX_PHDRS; ++i) {
		exe->phdr[i].p_vaddr = 0;
		exe->phdr[i].p_flags = 0;
	}

	for (i = 0, j = 0; i < nr; ++i) {
		if (phdr_tbl[i].p_type != PT_LOAD)
			continue;
		if (j == MAX_PHDRS)
			return 1;
		exe->phdr[j++] = phdr_tbl[i];
	}

	enum State {
		BEGIN,
		CODE,
		DATA,
		BRK
	} state = BEGIN;

	for (i = 0; i < MAX_PHDRS; ++i) {
		struct Elf32_Phdr *phdr = &exe->phdr[i];
		if (phdr->p_vaddr == 0)
			break;
		__u32 flags = PF_MASK & phdr->p_flags;
		switch (state) {
		case BEGIN:
			if (flags == PF_CODE) {
				exe->start_code = phdr->p_vaddr;
				exe->end_code = phdr->p_vaddr + phdr->p_memsz;
				state = CODE;
			} else {
				return 1;
			}
			break;
		case CODE:
			switch (flags) {
			case PF_CODE:
				exe->end_code = phdr->p_vaddr + phdr->p_memsz;
				break;
			case PF_RODATA:
				exe->start_data = phdr->p_vaddr;
				exe->end_data = phdr->p_vaddr + phdr->p_memsz;
				state = DATA;
				break;
			case PF_DATA:
				if (phdr->p_filesz > 0) {
					exe->start_data = phdr->p_vaddr;
					exe->end_data = phdr->p_vaddr + phdr->p_memsz;
					state = DATA;
				} else {
					exe->start_brk = phdr->p_vaddr;
					exe->brk = phdr->p_vaddr + phdr->p_memsz;
					state = BRK;
				}
				break;
			default:
				return 1;
			}
			break;
		case DATA:
			if (flags == PF_DATA) {
				if (phdr->p_filesz > 0) {
					exe->end_data = phdr->p_vaddr + phdr->p_memsz;
				} else {
					exe->start_brk = phdr->p_vaddr;
					exe->brk = phdr->p_vaddr + phdr->p_memsz;
					state = BRK;
				}
			} else {
				return 1;
			}
			break;
		case BRK:
			if (flags == PF_DATA) {
				if (phdr->p_filesz > 0)
					return 1;
				exe->brk = phdr->p_vaddr + phdr->p_memsz;
			} else {
				return 1;
			}
			break;
		}
	}

	// no code
	if (state == BEGIN)
		return 1;

	if (exe->start_data == 0)
		exe->start_data = exe->end_data = exe->end_code;
	if (exe->start_brk == 0)
		exe->start_brk = exe->brk = exe->end_data;

	return 0;
}

static inline int exec_read(struct file *file, void *buf, unsigned int count)
{
	return count != file->f_op->read(file->f_inode, file, buf, count);
}

int prep_exec(struct file *file, struct exec *exe)
{
	int error = 0;
	__u32 n;
	struct Elf32_Ehdr ehdr;
	struct Elf32_Phdr *phdr_tbl;

	// elf header
	file->f_pos = 0;
	if (exec_read(file, &ehdr, sizeof(ehdr)))
		return -EIO;
	if (check_elf_header(&ehdr))
		return -EINVAL;
	exe->entry = ehdr.e_entry;

	// program header table
	file->f_pos = ehdr.e_phoff;
	n = ehdr.e_phnum * ehdr.e_phentsize;
	phdr_tbl = (struct Elf32_Phdr *)kmalloc(n);
	if (!phdr_tbl)
		return -ENOMEM;
	if (exec_read(file, phdr_tbl, n)) {
		error = -EIO;
		goto tail;
	}
	if (fill_exec(phdr_tbl, ehdr.e_phnum, exe))
		error = -EINVAL;
tail:
	kfree(phdr_tbl);
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
		// Signal handlers are reset to their default actions, except for signals that are being ignored.
		if (current->sigaction[i].sa_handler != SIG_IGN)
			current->sigaction[i].sa_handler = SIG_DFL;
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
	int i;

	/* no turning back */
	flush_old_exec(exe);

	current->mm->start_code = exe->start_code;
	current->mm->end_code = exe->end_code;
	current->mm->start_data = exe->start_data;
	current->mm->end_data = exe->end_data;
	current->mm->start_brk = exe->start_brk;
	current->mm->brk = exe->brk;

	for (i = 0; i < MAX_PHDRS; ++i) {
		struct Elf32_Phdr *phdr = &exe->phdr[i];
		if (phdr->p_vaddr == 0)
			break;
		__u32 flags = PF_MASK & phdr->p_flags;
		switch (flags) {
		case PF_CODE:
			do_mmap(phdr->p_vaddr, phdr->p_memsz,
				PROT_READ | PROT_EXEC, MAP_FIXED | MAP_PRIVATE,
					fd, phdr->p_offset);
			break;
		case PF_RODATA:
			do_mmap(phdr->p_vaddr, phdr->p_memsz,
				PROT_READ, MAP_FIXED | MAP_PRIVATE,
					fd, phdr->p_offset);
			break;
		case PF_DATA:
			if (phdr->p_filesz > 0) {
				do_mmap(phdr->p_vaddr, phdr->p_memsz,
					PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE,
						fd, phdr->p_offset);
			} else {
				do_mmap(phdr->p_vaddr, phdr->p_memsz,
					PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
						-1, 0);
			}
			break;
		}
	}

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
	struct exec exe = {0};

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
	goto tail_1;

tail_2:
	for (i = 0; i < MAX_ARG_PAGES && exe.arg_page[i]; i++)
		page_free(exe.arg_page[i]);
tail_1:
	sys_close(fd); // close fd won't invalidate the memory mappings set up
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
