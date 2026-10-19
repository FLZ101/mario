#ifndef _SYSCALL_TRACE_H
#define _SYSCALL_TRACE_H

#include <types.h>

struct task_struct;

#define NR_SYSCALLS 512

struct syscall_trace_stat {
	uint64_t calls;
	uint64_t cycles;
	uint64_t max_cycles;
};

extern struct syscall_trace_stat syscall_trace[NR_SYSCALLS];

void syscall_trace_start(unsigned int nr);
void syscall_trace_end(unsigned int nr);
void syscall_trace_calibrate(void);
void syscall_trace_reset(void);
void syscall_trace_set_pid(pid_t pid);
void syscall_trace_task_exit(struct task_struct *task);
pid_t syscall_trace_get_pid(void);
unsigned long syscall_trace_elapsed_ms(void);
uint64_t syscall_trace_cycles_per_ms(void);
unsigned long syscall_trace_user_ms(void);
unsigned long syscall_trace_kernel_ms(void);
unsigned long syscall_trace_total_ms(void);
void syscall_trace_context_switch_out(void);
void syscall_trace_context_switch_in(void);
const char *syscall_trace_name(unsigned int nr);

#endif /* _SYSCALL_TRACE_H */
