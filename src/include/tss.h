#ifndef _TSS_H
#define _TSS_H

struct tss_struct {
	unsigned short	back_link, __blh;
	unsigned long	esp0;
	unsigned short	ss0, __ss0h;
	unsigned long	esp1;
	unsigned short	ss1, __ss1h;
	unsigned long	esp2;
	unsigned short	ss2, __ss2h;
	unsigned long	__cr3;
	unsigned long	eip;
	unsigned long	eflags;
	unsigned long	eax, ecx, edx, ebx;
	unsigned long	esp;
	unsigned long	ebp;
	unsigned long	esi;
	unsigned long	edi;
	unsigned short	es, __esh;
	unsigned short	cs, __csh;
	unsigned short	ss, __ssh;
	unsigned short	ds, __dsh;
	unsigned short	fs, __fsh;
	unsigned short	gs, __gsh;
	unsigned short	ldt, __ldth;
	unsigned short	trace, bitmap;
} __attribute__((gcc_struct, packed));

extern struct tss_struct init_tss;

#endif	/* _TSS_H */