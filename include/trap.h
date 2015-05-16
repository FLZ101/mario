#ifndef _TRAP_H
#define _TRAP_H

struct trap_frame {
	unsigned long 
	ebx,
	ecx,
	edx,
	esi,
	edi,
	ebp,
	eax,
	ds,
	es,
	error_code,
	eip,
	cs,
	eflags,
	esp,
	ss;
};

void trap_init(void);
void print_tr(struct trap_frame *tr);

#define userland(tr)	(3 & (tr)->cs)

#endif	/* _TRAP_H */