#include <ldt.h>
#include <misc.h>
#include <smp.h>
#include <task.h>

struct cpu_info cpu_info[NR_CPUS];

/*
 * Install the BSP cpu_info as the base of the per-CPU GDT descriptor and load
 * the matching selector into %gs.  From this point get_current() is no longer
 * derived from %esp and can later be per-CPU when APs get their own slots.
 */
static void set_percpu_gdt_base(unsigned long base)
{
	struct desc_struct *desc = gdt + GDT_ENTRY_PERCPU;

	desc->limit0 = 0xffff;
	desc->base0 = base & 0xffff;
	desc->base1 = (base >> 16) & 0xff;
	desc->type = 0x2;	/* writable, expand-up data segment */
	desc->s = 1;
	desc->dpl = 0;
	desc->p = 1;
	desc->limit1 = 0xf;
	desc->avl = 0;
	desc->l = 0;
	desc->d = 1;
	desc->g = 1;
	desc->base2 = (base >> 24) & 0xff;
}

void percpu_init(void)
{
	cpu_info[0].id = 0;
	cpu_info[0].online = 1;
	cpu_info[0].current_task = &init_task;

	set_percpu_gdt_base((unsigned long)&cpu_info[0]);
	__asm__ __volatile__(
		"movw %0, %%ax\n\t"
		"movw %%ax, %%gs"
		:
		: "i"(KERNEL_PERCPU_DS)
		: "ax");
}
