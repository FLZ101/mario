#include <mm/uaccess.h>
#include <mm/mm.h>

#include <errno.h>
#include <task.h>

int verify_area(int type, const void *addr, unsigned long size)
{
	struct vm_area_struct *vma;
	unsigned long start = (unsigned long)addr;

	/* kernel thread? */
	if (get_ds() == get_fs())
		return 0;

	vma = find_vma(current->mm, start);
	if (!vma)
		goto bad_area;
	if (vma->vm_start <= start)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (vma->vm_end - start > current->rlim[RLIMIT_STACK].rlim_cur)
		goto bad_area;
good_area:
	if (type == VERIFY_WRITE)
		goto check_write;
	while (1) {
		if (!(vma->vm_flags & VM_READ))
			goto bad_area;
		if (vma->vm_end - start >= size)
			return 0;
		if (!vma->vm_next|| vma->vm_end != vma->vm_next->vm_start)
			goto bad_area;
		vma = vma->vm_next;
	}
check_write:
	while (1) {
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
		if (vma->vm_end - start >= size)
			return 0;
		if (!vma->vm_next || vma->vm_end != vma->vm_next->vm_start)
			goto bad_area;
		vma = vma->vm_next;
	}
bad_area:
	return -EFAULT;
}

void memcpy_fromfs(void *to, const void *from, unsigned int n)
{
	__asm__ __volatile__ (
		"1:\n\t"
		"movb %%fs:(%1), %%al\n\t"
		"movb %%al, (%0)\n\t"
		"incl %0\n\t"
		"incl %1\n\t"
		"loop 1b"
		:
		:"D"(to), "S"(from), "c"(n)
		:"eax", "memory");
}

void memcpy_tofs(void *to, const void *from, unsigned int n)
{
	__asm__ __volatile__ (
		"1:\n\t"
		"movb (%1), %%al\n\t"
		"movb %%al, %%fs:(%0)\n\t"
		"incl %0\n\t"
		"incl %1\n\t"
		"loop 1b"
		:
		:"D"(to), "S"(from), "c"(n)
		:"eax", "memory");
}
