#ifndef _SMP_H
#define _SMP_H

/*
 * Keep the first three fields in this order: percpu_init() and entry.S rely
 * on CPU_INFO_CURRENT_TASK being 8 for the BSP before AP support lands.
 */
struct task_struct;

struct cpu_info {
	int id;
	int online;
	struct task_struct *current_task;
};

extern struct cpu_info bsp_cpu_info;

void percpu_init(void);

#endif	/* _SMP_H */
