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

#endif	/* _TRAP_H */