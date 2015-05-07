#include <mm/mm.h>
#include <lib/stddef.h>

struct vm_area_struct *find_vma(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap ; ; vma = vma->vm_next) {
		if (!vma)
			return NULL;
		if (vma->vm_end > addr)
			return vma;
	}
}

struct vm_area_struct *find_vma_intersection(struct mm_struct *mm,
	unsigned long start_addr, unsigned long end_addr)
{
	struct vm_area_struct *vma;

	vma = find_vma(mm, start_addr);
	if (!vma || end_addr <= vma->vm_start)
		return NULL;
	return vma;
}