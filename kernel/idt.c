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
		:"D"((void *)(idt+n)),\
		 "a"(((((unsigned long)dpl<<5)+0x80+type)<<8) + \
		 	((unsigned long)KERNEL_CS<<16)),\
		 "d"(offset)\
		:"memory");\
} while (0)

void __tinit set_intr_gate(unsigned int n, void *addr)
{
	_set_gate(n,14,0,addr);
}

void __tinit set_trap_gate(unsigned int n, void *addr)
{
	_set_gate(n,15,0,addr);
}

void __tinit set_system_gate(unsigned int n, void *addr)
{
	_set_gate(n,15,3,addr);
}

