#ifndef _SMP_H
#define _SMP_H

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

void percpu_init(void);
void smp_init(struct multiboot_info *m);
void smp_ap_entry(void);

#endif	/* _SMP_H */
