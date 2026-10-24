#include <misc.h>
#include <idt.h>

unsigned long long idt[256];

#define _set_gate(n, type, dpl, offset) \
do {\
	__asm__ __volatile__ (\
		"xchgw %%ax, %%dx\n\t"\
		"movl %%eax, (%%edi)\n\t"\
		"movl %%edx, 4(%%edi)"\
		:\
		:"D"((unsigned long)(idt+n)),\
		 "a"(((((unsigned long)dpl<<5)+0x80+type)<<8) + \
		 	((unsigned long)KERNEL_CS<<16)),\
		 "d"(offset)\
		:"memory");\
} while (0)

void set_intr_gate(unsigned int n, void *addr)
{
	_set_gate(n,14,0,addr);
}

void set_trap_gate(unsigned int n, void *addr)
{
	_set_gate(n,15,0,addr);
}

void set_system_gate(unsigned int n, void *addr)
{
	_set_gate(n,15,3,addr);
}

void load_idt(void)
{
	struct {
		uint16_t limit;
		unsigned long base;
	} __attribute__((gcc_struct, packed)) idtr = {
		.limit = sizeof(idt) - 1,
		.base = (unsigned long)idt,
	};

	/*
	 * Tell the compiler the inline asm reads idtr.  Without the memory
	 * clobber GCC may delete the two stores above at -O3 and make LIDT
	 * load whatever bytes happen to be in the stack slot.
	 */
	__asm__ __volatile__("lidt (%0)" : : "r"(&idtr) : "memory");
}

