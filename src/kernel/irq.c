#include <misc.h>
#include <idt.h>
#include <irq.h>
#include <io.h>

typedef void irq_handler(void);
extern irq_handler  
irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7,
irq8, irq9, irqa, irqb, irqc, irqd, irqe, irqf;

/*
 * Initialize the 8259A Programmable Interrupt Controller
 */
void __tinit i8259_init(void)
{
	/*
	 * Master PIC
	 */
	outb(0x20, 0x11);	/* ICW1 */
	outb(0x21, 0x20);	/* ICW2, map IRQ0~7 to int0x20~0x27 */
	outb(0x21, 0x04);	/* ICW3 */
	outb(0x21, 0x01);	/* ICW4, Non-AEOI */
	outb(0x21, 0x00);	/* OCW1, enable IRQ0~7 */

	/*
	 * Slave PIC
	 */
	outb(0xa0, 0x11);	/* ICW1 */
	outb(0xa1, 0x28);	/* ICW2, map IRQ8~15 to int0x28~0x2f */
	outb(0xa1, 0x02);	/* ICW3 */
	outb(0xa1, 0x01);	/* ICW4, Non-AEOI */
	outb(0xa1, 0x00);	/* OCW1, enable IRQ8~15 */
}

void __tinit irq_init(void)
{
	i8259_init();

	set_intr_gate(0x20, irq0);
	set_intr_gate(0x21, irq1);
	set_intr_gate(0x22, irq2);
	set_intr_gate(0x23, irq3);
	set_intr_gate(0x24, irq4);
	set_intr_gate(0x25, irq5);
	set_intr_gate(0x26, irq6);
	set_intr_gate(0x27, irq7);
	set_intr_gate(0x28, irq8);
	set_intr_gate(0x29, irq9);
	set_intr_gate(0x2a, irqa);
	set_intr_gate(0x2b, irqb);
	set_intr_gate(0x2c, irqc);
	set_intr_gate(0x2d, irqd);
	set_intr_gate(0x2e, irqe);
	set_intr_gate(0x2f, irqf);
}