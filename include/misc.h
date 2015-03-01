#ifndef _MISC_H
#define _MISC_H

#define KERNEL_CS   0x10
#define KERNEL_DS   0x18

#define USER_CS     0x23
#define USER_DS     0x2b

#ifdef __ASSEMBLY__

#define KERNEL_BASE 0x80000000

#define __phy(x)	((x) - KERNEL_BASE)
#define __vir(x)	((x) + KERNEL_BASE)

#define ENTRY(name) \
 	.globl _##name; \
 	_##name##:

#else /* __ASSEMBLY__ */

/*
 * user space: 0 ~ 2G-1
 * kernel space: 2G ~ 4G-1
 */
#define KERNEL_BASE	0x80000000UL /* 2G */

/* convert in kernel space virtual address to physical address */
#define __phy(x)	((unsigned long)(x) - KERNEL_BASE)
/* convert in kernel space physical address to virtual address */
#define __vir(x)	((unsigned long)(x) + KERNEL_BASE)

#define PAGE_SHIFT	12
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE-1))

#define PFN_UP(x)	(((unsigned long)(x) + PAGE_SIZE-1) >> PAGE_SHIFT)
#define PFN_DOWN(x)	((unsigned long)(x) >> PAGE_SHIFT)

#define SYSCALL_VECTOR	0x80

/* Mark code as being only used at initialization time */
#define __tinit __attribute__ ((section (".tinit")))
/* Mark data as being only used at initialization time */
#define __dinit __attribute__ ((section (".dinit")))

#include <multiboot.h>

void early_print_init(struct multiboot_info *m);
void early_print(const char *fmt, ...);

#define early_hang(...) do {\
	early_print(__VA_ARGS__);\
	__asm__ __volatile__ ("cli; 1:hlt; jmp 1b");\
} while (0)

#define cli() __asm__ __volatile__ ("cli": : :"memory")
#define sti() __asm__ __volatile__ ("sti": : :"memory")

#define save_flags(x) \
__asm__ __volatile__("pushfl; popl %0":"=g"(x): :"memory")

#define restore_flags(x) \
__asm__ __volatile__("pushl %0; popfl": :"g"(x):"memory")

/*
 * On the Intel 386, the fastcall attribute causes the compiler to pass 
 * the first argument (if of integral type) in the register ECX and the 
 * second argument (if of integral type) in the register EDX. Subsquent 
 * and other typed arguments are passed on the stack. The called function 
 * will pop the arguments off the stack. If the number of arguments is 
 * variable all arguments are pushed on the stack. 
 * Refer to 'Using the GNU Compiler Collection (For GCC version 4.7.1)'.
 */
#define FASTCALL __attribute__((fastcall))

#define save_segment(seg,value) \
	asm volatile("movw %%" #seg ",%0" :"=m"(value))

#define load_segment(seg,value) \
	asm volatile("movw %0, %%" #seg : :"m"(value))

#endif	/* __ASSEMBLY__ */

#endif	/* _MISC_H */
