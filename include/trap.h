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
	ods,
	oes,
	error_code,
	eip,
	ocs,
	eflags,
	esp,
	oss;
};

void trap_init(void);

#endif	/* _TRAP_H */