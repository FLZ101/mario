#ifndef _IDT_H
#define _IDT_H

void set_intr_gate(unsigned int n, void *addr);
void set_trap_gate(unsigned int n, void *addr);
void set_system_gate(unsigned int n, void *addr);

void isr_init(void);
void irq_init(void);

#endif	/* _IDT_H */