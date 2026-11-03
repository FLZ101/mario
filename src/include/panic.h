#ifndef _PANIC_H
#define _PANIC_H

#include <types.h>

/* Register state captured by hang() before it calls any C code. */
struct panic_regs {
	unsigned long eax;
	unsigned long ebx;
	unsigned long ecx;
	unsigned long edx;
	unsigned long esi;
	unsigned long edi;
	unsigned long ebp;
	unsigned long esp;
	unsigned long eip;
	unsigned long eflags;
};

struct panic_site {
	const char *file;
	const char *function;
	unsigned int line;
};

void panic_serial_init(void);
void panic_hang(const struct panic_regs *regs, const struct panic_site *site,
		const char *format, ...) __attribute__((noreturn));

/*
 * Capture the caller's state in place.  This must remain a macro: invoking a
 * helper function would modify the registers and stack we are trying to dump.
 */
#define panic_capture_regs(regs) do { \
	__asm__ __volatile__( \
		"movl %%eax, %0\n\t" \
		"movl %%ebx, %1\n\t" \
		"movl %%ecx, %2\n\t" \
		"movl %%edx, %3\n\t" \
		"movl %%esi, %4\n\t" \
		"movl %%edi, %5\n\t" \
		"movl %%ebp, %6\n\t" \
		"movl %%esp, %7\n\t" \
		"pushfl\n\t" \
		"popl %%eax\n\t" \
		"movl %%eax, %9\n\t" \
		"call 1f\n\t" \
		"1: popl %%eax\n\t" \
		"movl %%eax, %8\n\t" \
		: "=m" ((regs).eax), "=m" ((regs).ebx), \
		  "=m" ((regs).ecx), "=m" ((regs).edx), \
		  "=m" ((regs).esi), "=m" ((regs).edi), \
		  "=m" ((regs).ebp), "=m" ((regs).esp), \
		  "=m" ((regs).eip), "=m" ((regs).eflags) \
		: : "eax", "memory"); \
} while (0)

#endif /* _PANIC_H */
