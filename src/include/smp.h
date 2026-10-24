#ifndef _SMP_H
#define _SMP_H

#include <tss.h>

/* Maximum number of CPUs supported by this SMP bring-up phase. */
#define NR_CPUS 8

/*
 * Keep the first three fields in this order: percpu_init() and entry.S rely
 * on CPU_INFO_CURRENT_TASK being 8 for the BSP before AP support lands.
 */
struct task_struct;
struct multiboot_info;

/*
 * Keep the first three fields in this order: percpu_init(), entry.S, and
 * _switch_to() rely on CPU_INFO_CURRENT_TASK being 8.
 */
struct cpu_info {
	int id;
	int online;
	struct task_struct *current_task;
};

extern struct cpu_info cpu_info[NR_CPUS];
#define bsp_cpu_info cpu_info[0]

/* Number of CPUs discovered and (successfully) brought online. */
extern int nr_cpu_ids;

/* Per-AP TSSs, indexed by logical CPU number.  CPU 0 keeps init_tss. */
extern struct tss_struct ap_tss[NR_CPUS];

struct desc_struct;

void percpu_init(void);
void smp_init(struct multiboot_info *m);
void smp_ap_entry(void);
void smp_send_reschedule(int cpu);
void smp_send_tlb_flush(int cpu);
struct task_struct *smp_idle_task(int cpu);
struct desc_struct *cpu_gdt(int cpu);

/* Mask of all CPUs that have successfully come online. */
static inline unsigned long online_cpu_mask(void)
{
	return nr_cpu_ids ? (1UL << nr_cpu_ids) - 1 : 1;
}

#endif	/* _SMP_H */
