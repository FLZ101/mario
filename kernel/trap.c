#include <misc.h>
#include <trap.h>
#include <idt.h>

void divide_error(void);

void __tinit trap_init(void)
{
	set_trap_gate(0, divide_error);
}

char *divide_error_msg = "Divide Error\n";

__asm__(
		"_divide_error:\n\t"
		"pushl %es\n\t"
		"pushl %ds\n\t"
		"pushal\n\t"
		"movl $0x18, %eax\n\t"
		"movw %ax, %es\n\t"
		"movw %ax, %ds\n\t"
		"pushl _divide_error_msg\n\t"
		"call _early_print\n\t"
		"addl $4,  %esp\n\t"
		"popal\n\t"
		"popl %ds\n\t"
		"popl %es\n\t"
		"iret"
       );
