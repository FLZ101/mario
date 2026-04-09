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

#define MAX_PHDRS 16

struct exec {
	char *comm;
	struct Elf32_Phdr phdr[MAX_PHDRS];
	unsigned long entry;
	unsigned long at_phdr, at_phent, at_phnum;
	int argc, envc;
	unsigned long arg_page[MAX_ARG_PAGES], arg_p;

	unsigned long start_code, end_code, start_data, end_data, start_brk, brk;
};

extern int do_openat(int dirfd, char *filename, int flags);

int open_execat(int dirfd, char *filename)
{
	int fd;
	struct file *file;
	struct inode *inode;

	fd = do_openat(dirfd, filename, O_RDONLY);
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
		if (phdr_tbl[i].p_type == PT_PHDR) {
			exe->at_phdr = phdr_tbl[i].p_vaddr;
			continue;
		}
		if (phdr_tbl[i].p_type != PT_LOAD)
			continue;
		if (j == MAX_PHDRS) {
			printk("Too many program headers!\n");
			return -EINVAL;
		}
		exe->phdr[j++] = phdr_tbl[i];
	}

	if (!exe->at_phdr) {
		printk("No PT_PHDR program header found!\n");
		return -EINVAL;
	}

	enum State {
		BEGIN,
		CODE,
		DATA
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
			case PF_DATA:
				exe->start_data = phdr->p_vaddr;
				exe->end_data = phdr->p_vaddr + phdr->p_memsz;
				state = DATA;
				break;
			default:
				return 1;
			}
			break;
		case DATA:
			if (flags == PF_RODATA || flags == PF_DATA) {
				exe->end_data = phdr->p_vaddr + phdr->p_memsz;
			} else {
				return 1;
			}
			break;
		}
	}

	if (state == BEGIN) {
		printk("No code!\n");
		return -EINVAL;
	}

	if (exe->start_data == 0)
		exe->start_data = exe->end_data = PAGE_ALIGN(exe->end_code);
	exe->start_brk = exe->brk = PAGE_ALIGN(exe->end_data);

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
	exe->at_phent = ehdr.e_phentsize;
	exe->at_phnum = ehdr.e_phnum;
	exe->at_phdr = 0;

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
	error = fill_exec(phdr_tbl, ehdr.e_phnum, exe);
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
		if (error == -ENOENT) // p is "". Just ignore
			return 0;

		if (error != -ENOMEM)
			error = -EFAULT;
		return error;
	}
	len = strlen(tmp) + 1;
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

	// Use get_zero_page (rather than page_alloc):
	//   * the page should be zeroed
	//   * for userspace pages (shared between forks), use get/put wrapper (rather than alloc/free) for reference counting
	//
	if (!(exe->arg_page[0] = get_zero_page()))
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

extern int set_page(struct mm_struct *mm, unsigned long page, unsigned long addr);

void setup_aux_vector(struct exec *exe, auxv_t *av)
{
	av->a_type = AT_PHDR;
	av->a_ptr = (void *) exe->at_phdr;
	++av;

	av->a_type = AT_PHENT;
	av->a_val = exe->at_phent;
	++av;

	av->a_type = AT_PHNUM;
	av->a_val = exe->at_phnum;
	++av;

	av->a_type = AT_PAGESZ;
	av->a_val = PAGE_SIZE;
	++av;
}

// Return number of words needed to store argc, argv[], envp[] and aux vector
static inline int get_n_word(struct exec *exe)
{
	return 1 + (exe->argc + 1) + (exe->envc + 1) + (AT_VECTOR_SIZE * 2);
}

extern void oom(struct task_struct *);

int setup_arg_pages(struct exec *exe)
{
	int i;
	char *pc;
	unsigned long nr_page, start_stack, stack_page, *p1, *p2;

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
	if (!stack_page) {
		oom(current);
		goto tail_1;
	}
	if (set_page(current->mm, stack_page, start_stack - PAGE_SIZE)) {
		goto tail_2;
	}

	unsigned n_word = get_n_word(exe);
	assert(n_word * sizeof(long) <= PAGE_SIZE);

	p1 = (unsigned long *)start_stack - n_word;
	/* argc */
	*(p1) = exe->argc;

	pc = (char *)start_stack;
	current->mm->arg_start = (unsigned long)pc;

	/* arguments */
	p2 = p1 + 1;
	for (i = 0; i < exe->argc; i++) {
		*(p2++) = (unsigned long)pc;
		while (*(pc++))
			;
	}
	*(p2++) = 0;
	current->mm->arg_end = current->mm->env_start = (unsigned long)pc;

	/* environment variables */
	for (i = 0; i < exe->envc; i++) {
		*(p2++) = (unsigned long)pc;
		while (*(pc++))
			;
	}
	*(p2++) = 0;
	current->mm->env_end = (unsigned long)pc;

	setup_aux_vector(exe, (auxv_t *)p2);

	/* mmap arguments and environment variables */
	do_mmap(start_stack, KERNEL_BASE - start_stack, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	/* mmap stack */
	do_mmap(start_stack - PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | VM_GROWSDOWN, -1, 0);
	return 0;

tail_2:
	put_page(stack_page);
tail_1:
	for (i = 0; i < MAX_ARG_PAGES; i++)
		if (exe->arg_page[i])
			put_page(exe->arg_page[i]);
	return -ENOMEM;
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
	int err;

	if (get_n_word(exe) * sizeof(long) > PAGE_SIZE) {
		printk("Unable to fit argc, argv, envp and aux vector into one page!\n");
		return -EINVAL;
	}

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
		assert(PAGE_ALIGNED(phdr->p_vaddr));

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
				do_mmap(phdr->p_vaddr, phdr->p_filesz,
					PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE,
						fd, phdr->p_offset);

				// .data and .bss may be in the same segment. For example,
				//
				// readelf --segments app/init/init.exe
				//
				// Program Headers:
				//   Type           Offset   VirtAddr   PhysAddr   FileSiz MemSiz  Flg Align
				//   LOAD           0x001000 0x00401000 0x00401000 0x03000 0x03000 R E 0x1000
				//   LOAD           0x004000 0x00404000 0x00404000 0x00014 0x014ec RW  0x1000
				//
				//  Section to Segment mapping:
				//   Segment Sections...
				//    00     .text
				//    01     .data .bss

				size_t sz = PAGE_ALIGN(phdr->p_filesz);
				if (phdr->p_memsz > sz) {
					do_mmap(phdr->p_vaddr + sz, phdr->p_memsz - sz,
						PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
							-1, 0);
				}
			} else {
				do_mmap(phdr->p_vaddr, phdr->p_memsz,
					PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS,
						-1, 0);
			}
			break;
		}
	}

	err = setup_arg_pages(exe);
	if (err)
		return err;

	current->did_exec = 1;

	start_thread(tr, exe->entry,
		current->mm->start_stack - 4 * get_n_word(exe));
	return 0;
}

int do_execveat(int dirfd, char *filename, char **argv, char **envp, struct trap_frame *tr)
{
	int i, fd, error;
	struct file *file;
	struct exec exe = {0};

	fd = open_execat(dirfd, filename);
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
	if (error)
		goto tail_2;
	goto tail_1;

tail_2:
	for (i = 0; i < MAX_ARG_PAGES && exe.arg_page[i]; i++)
		put_page(exe.arg_page[i]);
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
	error = do_execveat(AT_FDCWD, filename, (char **)tr.ecx, (char **)tr.edx, &tr);
	putname(filename);
	return error;
}

int sys_execveat(struct trap_frame tr)
{
	int error;
	char *filename;

	error = getname((char *)tr.ecx, &filename);
	if (error)
		return error;
	error = do_execveat(tr.ebx, filename, (char **)tr.edx, (char **)tr.esi, &tr);
	putname(filename);
	return error;
}
