#ifndef _MM_H
#define _MM_H

#include <mm/paging.h>

#include <lib/list.h>

struct vm_area_struct;
struct mm_struct {
	struct vm_area_struct *mmap;
	struct vm_area_struct *mmap_cache;
	int map_count;
	pde_t *pd;
	struct list_head mmlist;
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;
};

struct vm_area_struct {
	struct mm_struct *vm_mm;
	unsigned long vm_start, vm_end;
	struct vm_area_struct *vm_next;
	unsigned long vm_flags;
};

#endif /* _MM_H */