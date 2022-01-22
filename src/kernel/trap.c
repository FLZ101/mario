#include <misc.h>
#include <trap.h>
#include <idt.h>
#include <signal.h>
#include <sched.h>

void print_tr(struct trap_frame *tr)
{
	early_print("ebx=%x, ecx=%x, edx=%x, esi=%x\n",
		tr->ebx, tr->ecx, tr->edx, tr->esi);
	early_print("edi=%x, ebp=%x, eax=%x,  ds=%x\n",
		tr->edi, tr->ebp, tr->eax, tr->ds);
	early_print(" es=%x, err=%x, eip=%x,  cs=%x\n", 
		tr->es, tr->error_code, tr->eip, tr->cs);
	early_print("elf=%x, esp=%x,  ss=%x\n",
		tr->eflags, tr->esp, tr->ss);
}

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

static void die(char *str, struct trap_frame *tr, long err)
{
	early_print("[%u] %s\n", err, str);
	print_tr(tr);
	do_exit(SIGSEGV);
}

void die_if_kernel(char *str, struct trap_frame *tr, long err)
{
	if (!userland(tr))
		die(str, tr, err);
}

#define DO_ERROR(trapnr, signr, str, name, tsk) \
void do_##name(struct trap_frame* tr, long error_code) \
{ \
	tsk->thread.error_code = error_code; \
	tsk->thread.trap_no = trapnr; \
	send_sig(signr, tsk, 1); \
	die_if_kernel(str, tr, error_code); \
}

DO_ERROR( 0, SIGFPE,  "divide error", divide_error, current)
DO_ERROR( 3, SIGTRAP, "int3", int3, current)
DO_ERROR( 4, SIGSEGV, "overflow", overflow, current)
DO_ERROR( 5, SIGSEGV, "bounds", bounds, current)
DO_ERROR( 6, SIGILL,  "invalid operand", invalid_op, current)
DO_ERROR( 7, SIGSEGV, "device not available", device_not_available, current)
DO_ERROR( 8, SIGSEGV, "double fault", double_fault, current)
DO_ERROR( 9, SIGFPE,  "coprocessor segment overrun", coprocessor_segment_overrun, current)
DO_ERROR(10, SIGSEGV, "invalid TSS", invalid_TSS, current)
DO_ERROR(11, SIGBUS,  "segment not present", segment_not_present, current)
DO_ERROR(12, SIGBUS,  "stack segment", stack_segment, current)
DO_ERROR(15, SIGSEGV, "reserved", reserved, current)
DO_ERROR(17, SIGSEGV, "alignment check", alignment_check, current)

void do_general_protection(struct trap_frame *tr, long error_code)
{
	die_if_kernel("general protection", tr, error_code);
	current->thread.error_code = error_code;
	current->thread.trap_no = 13;
	send_sig(SIGSEGV, current, 1);
}

void do_nmi(struct trap_frame *tr, long error_code)
{
	early_print("[WARN] NMI received\n");
	print_tr(tr);
}

void do_debug(struct trap_frame *tr, long error_code)
{
	send_sig(SIGTRAP, current, 1);
	current->thread.trap_no = 1;
	current->thread.error_code = error_code;
	if (!userland(tr)) {
		/* If this is a kernel mode trap, then reset db7 and allow us to continue */
		__asm__("movl %0,%%db7"
			: /* no output */
			: "r" (0));
		return;
	}
	die_if_kernel("debug", tr, error_code);
}

void do_coprocessor_error(struct trap_frame *tr, long error_code)
{
	die("[ERROR] coprocessor error", tr, error_code);
}

void do_spurious_interrupt_bug(struct trap_frame *tr, long error_code)
{
	/* Empty */
}
