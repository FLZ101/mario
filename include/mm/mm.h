#ifndef _MM_H
#define _MM_H

#include <mm/paging.h>

#include <lib/list.h>

struct vm_area_struct;

struct mm_struct {
	struct vm_area_struct *mmap;
	struct vm_area_struct *mmap_cache;
	int mm_count;
	pde_t *pd;
	struct list_head mmlist;
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;
};

struct vm_operations_struct {
	void (*open)(struct vm_area_struct *vma);
	void (*close)(struct vm_area_struct *vma);
};

struct file;
struct vm_area_struct {
	struct mm_struct *vm_mm;
	unsigned long vm_start, vm_end;
	/* linked list of VM areas per task, sorted by address */
	struct vm_area_struct *vm_next;
	unsigned long vm_flags;
	struct file *vm_file;
	struct vm_operations_struct *vm_ops;
};

/* vm_flags... */
#define VM_READ		0x0001
#define VM_WRITE	0x0002
#define VM_EXEC		0x0004
#define VM_SHARED	0x0008

#define VM_MAYREAD	0x0010
#define VM_MAYWRITE	0x0020
#define VM_MAYEXEC	0x0040
#define VM_MAYSHARE	0x0080

#define VM_GROWSDOWN	0x0100
#define VM_GROWSUP	0x0200
#define VM_DENYWRITE	0x0800

#define VM_EXECUTABLE	0x1000

struct vm_area_struct *find_vma(struct mm_struct *, unsigned long);

#endif /* _MM_H */