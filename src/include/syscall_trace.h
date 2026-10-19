#ifndef _SYSCALL_TRACE_H
#define _SYSCALL_TRACE_H

#include <types.h>

#define NR_SYSCALLS 512

struct syscall_trace_stat {
	uint64_t calls;
	uint64_t cycles;
	uint64_t max_cycles;
};

extern struct syscall_trace_stat syscall_trace[NR_SYSCALLS];

void syscall_trace_start(unsigned int nr);
void syscall_trace_end(unsigned int nr);
void syscall_trace_reset(void);
void syscall_trace_set_pid(pid_t pid);
pid_t syscall_trace_get_pid(void);
void syscall_trace_context_switch_out(void);
void syscall_trace_context_switch_in(void);
const char *syscall_trace_name(unsigned int nr);

#endif /* _SYSCALL_TRACE_H */
