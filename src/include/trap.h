#ifndef _TRAP_H
#define _TRAP_H

struct trap_frame {
	long ebx;
	long ecx;
	long edx;
	long esi;
	long edi;
	long ebp;
	long eax;
	unsigned short ds, __dsu;
	unsigned short es, __esu;
	long error_code;
	long eip;
	unsigned short cs, __csu;
	long eflags;
	long esp;
	unsigned short ss, __ssu;
};

void trap_init(void);
void print_tr(struct trap_frame *tr);

#define userland(tr)	(3 & (tr)->cs)

#endif	/* _TRAP_H */
