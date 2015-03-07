#include <misc.h>
#include <trap.h>
#include <idt.h>

typedef void trap_handler(void);
extern trap_handler
divide_error,
debug,
nmi,
int3,
overflow,
bounds,
invalid_op,
device_not_available,
double_fault,
coprocessor_segment_overrun,
invalid_TSS,
segment_not_present,
stack_segment,
general_protection,
page_fault,
spurious_interrupt_bug,
coprocessor_error,
system_call;

void __tinit trap_init(void)
{
	set_trap_gate(0, divide_error);
	set_trap_gate(1, debug);
	set_trap_gate(2, nmi);
	/*
	 * int3~5 can be triggered from userland
	 */
	set_system_gate(3, int3);
	set_system_gate(4, overflow);
	set_system_gate(5, bounds);
	set_intr_gate(6, invalid_op);
	set_trap_gate(7, device_not_available);
	set_trap_gate(8, double_fault);
	set_trap_gate(9, coprocessor_segment_overrun);
	set_trap_gate(10, invalid_TSS);
	set_trap_gate(11, segment_not_present);
	set_trap_gate(12, stack_segment);
	set_trap_gate(13, general_protection);
	set_trap_gate(14, page_fault);
	set_trap_gate(15, spurious_interrupt_bug);
	set_trap_gate(16, coprocessor_error);

	set_system_gate(SYSCALL_VECTOR, system_call);
}

void do_divide_error(struct trap_frame *tr)
{
	early_print("Divide Error\n");
}

void do_debug(struct trap_frame *tr)
{
	early_print("Debug\n");
}

void do_nmi(struct trap_frame *tr)
{
	early_print("NMI\n");
}

void do_int3(struct trap_frame *tr)
{
	early_print("Int3\n");
}

void do_overflow(struct trap_frame *tr)
{
	early_print("Overflow\n");
}

void do_bounds(struct trap_frame *tr)
{
	early_print("Bounds\n");
}

void do_invalid_op(struct trap_frame *tr)
{
	early_hang("Invalid Opcode\n");
}

void do_device_not_available(struct trap_frame *tr)
{
	early_print("Device Not Available\n");
}

void do_double_fault(struct trap_frame *tr)
{
	early_print("Double Fault\n");
}

void do_coprocessor_segment_overrun(struct trap_frame *tr)
{
	early_print("Coprocessor Segment Overrun\n");
}

void do_invalid_TSS(struct trap_frame *tr)
{
	early_print("Invalid TSS\n");
}

void do_segment_not_present(struct trap_frame *tr)
{
	early_print("Segment Not Present\n");
}

void do_stack_segment(struct trap_frame *tr)
{
	early_print("Stack Segment\n");
}

void do_general_protection(struct trap_frame *tr)
{
	early_hang("General Protection\n");
}

void do_page_fault(struct trap_frame *tr)
{
	early_print("Page Fault\n");
	while (1);
}

void do_spurious_interrupt_bug(struct trap_frame *tr)
{
	early_print("Spurious Interrupt Bug\n");
}

void do_coprocessor_error(struct trap_frame *tr)
{
	early_print("Coprocessor Error\n");
}