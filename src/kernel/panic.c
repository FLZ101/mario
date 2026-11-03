#include <panic.h>

#include <fs/tty/serial.h>
#include <io.h>
#include <misc.h>
#include <printk.h>
#include <task.h>

#include <lib/stdarg.h>

extern char _text[];
extern char _etext[];

#define PANIC_UART_POLL_LIMIT	1000000U
#define PANIC_MESSAGE_SIZE	256
#define PANIC_STACK_DEPTH	32

struct stack_frame {
	unsigned long previous_ebp;
	unsigned long return_address;
};

static volatile unsigned int panic_owner;
static volatile unsigned int panic_context_ready;
static char panic_message[PANIC_MESSAGE_SIZE];

/* Configure COM1 without depending on serial tty registration. */
void panic_serial_init(void)
{
	outb(COM1 + 1, 0x00); /* Disable UART interrupts. */
	outb(COM1 + 3, 0x80); /* Enable divisor latch access. */
	outb(COM1 + 0, 0x03); /* 38400 baud divisor, low byte. */
	outb(COM1 + 1, 0x00); /* 38400 baud divisor, high byte. */
	outb(COM1 + 3, 0x03); /* 8 data bits, no parity, one stop bit. */
	outb(COM1 + 2, 0xC7); /* Enable and clear FIFOs. */
	outb(COM1 + 4, 0x03); /* Assert DTR and RTS without IRQ delivery. */
	/* percpu_init() completed before this call, so current is now usable. */
	panic_context_ready = 1;
}

/* Never wait forever for a failed UART while handling a fatal kernel error. */
static void panic_putc_raw(unsigned char c)
{
	unsigned int polls = PANIC_UART_POLL_LIMIT;

	while (polls--) {
		if (inb(COM1 + 5) & 0x20) {
			outb(COM1, c);
			return;
		}
	}
}

static void panic_putc(unsigned char c)
{
	if (c == '\n')
		panic_putc_raw('\r');
	panic_putc_raw(c);
}

static void panic_puts(const char *text)
{
	if (!text) {
		panic_puts("(null)");
		return;
	}
	while (*text)
		panic_putc(*text++);
}

static void panic_put_hex(unsigned long value)
{
	static const char digits[] = "0123456789abcdef";

	for (int shift = 28; shift >= 0; shift -= 4)
		panic_putc(digits[(value >> shift) & 0xf]);
}

static void panic_put_unsigned(unsigned long value)
{
	char digits[10];
	int count = 0;

	if (!value) {
		panic_putc('0');
		return;
	}
	while (value) {
		digits[count++] = '0' + value % 10;
		value /= 10;
	}
	while (count)
		panic_putc(digits[--count]);
}

static void panic_put_task_name(const char *name)
{
	for (int index = 0; index < 16 && name[index]; ++index)
		panic_putc(name[index]);
}

static int panic_claim_owner(void)
{
	unsigned int owner = 1;

	/* xchg with memory is atomic on x86, including across CPUs. */
	__asm__ __volatile__("xchgl %0, %1"
			     : "+r"(owner), "+m"(panic_owner)
			     : : "memory");
	return owner == 0;
}

static void panic_halt_forever(void) __attribute__((noreturn));

static void panic_halt_forever(void)
{
	for (;;) {
		__asm__ __volatile__("hlt" : : : "memory");
	}
}

static int frame_fits(unsigned long frame, unsigned long stack_bottom,
		      unsigned long stack_top)
{
	return !(frame & (sizeof(unsigned long) - 1)) &&
	       frame >= stack_bottom &&
	       frame <= stack_top - sizeof(struct stack_frame);
}

static int stack_bounds(const struct task_struct *task, unsigned long esp,
			unsigned long *stack_bottom, unsigned long *stack_top)
{
	unsigned long top = task->thread.esp0;

	if (top < KERNEL_BASE + KSTACK_SIZE)
		return 0;
	*stack_top = top;
	*stack_bottom = top - KSTACK_SIZE;
	return esp >= *stack_bottom && esp < *stack_top;
}

static void panic_dump_stack(const struct panic_regs *regs,
			     const struct task_struct *task)
{
	unsigned long stack_bottom;
	unsigned long stack_top;
	unsigned long frame = regs->ebp;

	panic_puts("stack:\n");
	if (!task) {
		panic_puts("  unavailable (early boot)\n");
		return;
	}
	if (!stack_bounds(task, regs->esp, &stack_bottom, &stack_top)) {
		panic_puts("  unavailable (invalid kernel stack bounds)\n");
		return;
	}

	for (unsigned int depth = 0; depth < PANIC_STACK_DEPTH; ++depth) {
		struct stack_frame *current_frame;
		unsigned long next;
		unsigned long return_address;

		if (!frame_fits(frame, stack_bottom, stack_top)) {
			panic_puts("  stopped (invalid frame)\n");
			return;
		}
		current_frame = (struct stack_frame *)frame;
		next = current_frame->previous_ebp;
		return_address = current_frame->return_address;

		panic_puts("  #");
		panic_put_unsigned(depth);
		panic_puts(" ");
		panic_put_hex(return_address);
		if (return_address < (unsigned long)_text ||
		    return_address >= (unsigned long)_etext) {
			panic_puts(" (outside kernel text)\n");
			return;
		}
		panic_putc('\n');

		if (next <= frame || !frame_fits(next, stack_bottom, stack_top)) {
			panic_puts("  stopped (end of frame chain)\n");
			return;
		}
		frame = next;
	}
	panic_puts("  stopped (frame limit)\n");
}

static void panic_dump_regs(const struct panic_regs *regs)
{
	panic_puts("regs: eip=");
	panic_put_hex(regs->eip);
	panic_puts(" eflags=");
	panic_put_hex(regs->eflags);
	panic_puts(" esp=");
	panic_put_hex(regs->esp);
	panic_putc('\n');

	panic_puts("regs: eax=");
	panic_put_hex(regs->eax);
	panic_puts(" ebx=");
	panic_put_hex(regs->ebx);
	panic_puts(" ecx=");
	panic_put_hex(regs->ecx);
	panic_puts(" edx=");
	panic_put_hex(regs->edx);
	panic_putc('\n');

	panic_puts("regs: esi=");
	panic_put_hex(regs->esi);
	panic_puts(" edi=");
	panic_put_hex(regs->edi);
	panic_puts(" ebp=");
	panic_put_hex(regs->ebp);
	panic_putc('\n');
}

void panic_hang(const struct panic_regs *regs, const struct panic_site *site,
		const char *format, ...)
{
	struct task_struct *task;
	va_list args;

	/* Keep the first panic report intact if another CPU or a fault recurses. */
	cli();
	if (!panic_claim_owner())
		panic_halt_forever();

	va_start(args, format);
	vsprintkn(panic_message, sizeof(panic_message), format, args);
	va_end(args);

	task = panic_context_ready ? current : NULL;
	panic_puts("\n=== MARIO FATAL HANG ===\n");
	if (task) {
		panic_puts("cpu=");
		panic_put_unsigned(task->cpu);
		panic_puts(" pid=");
		panic_put_unsigned(task->pid);
		panic_puts(" task=");
		panic_put_task_name(task->comm);
		panic_putc('\n');
	} else {
		panic_puts("cpu/task: unavailable (early boot)\n");
	}
	panic_puts("reason: ");
	panic_puts(panic_message);
	unsigned int length = 0;

	while (length < sizeof(panic_message) && panic_message[length])
		length++;
	if (!length || panic_message[length - 1] != '\n')
		panic_putc('\n');
	panic_puts("site: ");
	panic_puts(site->file);
	panic_putc(':');
	panic_put_unsigned(site->line);
	panic_putc(' ');
	panic_puts(site->function);
	panic_putc('\n');
	panic_dump_regs(regs);
	panic_dump_stack(regs, task);
	panic_puts("halted\n");
	panic_halt_forever();
}
