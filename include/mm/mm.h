#ifndef _MM_H
#define _MM_H

#include <mm/pagetable.h>

#include <lib/list.h>

void paging_init(void);

#define flush_tlb() \
do { \
	asm volatile("movl %%cr3,%%eax; movl %%eax,%%cr3":::"eax"); \
} while (0)

#define switch_pd(pd) \
do { \
	asm volatile("movl %0,%%cr3": :"r"(__phy(pd))); \
} while (0)

struct vm_area_struct;
struct mm_struct {
	pde_t *pd;
	struct vm_area_struct *mmap;
	struct vm_area_struct *mmap_cache;
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;
};

struct vm_operations_struct {
	void (*open)(struct vm_area_struct *);
	void (*close)(struct vm_area_struct *);
	void (*sync)(struct vm_area_struct *, unsigned long, unsigned long, int);
	void (*unmap)(struct vm_area_struct *, unsigned long, unsigned long);
	int (*nopage)(struct vm_area_struct *, unsigned long, unsigned long);
	int (*wppage)(struct vm_area_struct *, unsigned long, unsigned long);
};

struct file;
struct vm_area_struct {
	struct mm_struct *vm_mm;
	unsigned long vm_start, vm_end;
	/* linked list of VM areas per task, sorted by address */
	struct vm_area_struct *vm_next;
	/* for VM_SHARED areas */
	struct vm_area_struct *vm_next_share;
	struct vm_area_struct *vm_prev_share;

	unsigned long vm_flags;
	/* attributes of pages in this vm_area */
	unsigned long vm_page_prot;
	struct inode *vm_inode;
	unsigned long vm_offset;
	struct vm_operations_struct *vm_ops;
};

/*
 * vm_flags...
 */
#define VM_READ		0x0001	/* currently active flags */
#define VM_WRITE	0x0002
#define VM_EXEC		0x0004
#define VM_SHARED	0x0008

#define VM_MAYREAD	0x0010	/* limits for mprotect() etc */
#define VM_MAYWRITE	0x0020
#define VM_MAYEXEC	0x0040
#define VM_MAYSHARE	0x0080

#define VM_GROWSDOWN	0x0100	/* general info on the segment */
#define VM_GROWSUP	0x0200

struct vm_area_struct *find_vma(struct mm_struct *, unsigned long);

int generic_file_mmap(struct inode *, struct file *, struct vm_area_struct *);

void print_mmap(struct mm_struct *mm);

#endif /* _MM_H */