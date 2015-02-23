#ifndef _MM_H
#define _MM_H

struct mm_struct {
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long start_brk, brk, start_stack;
	unsigned long arg_start, arg_end, env_start, env_end;
};

#endif /* _MM_H */