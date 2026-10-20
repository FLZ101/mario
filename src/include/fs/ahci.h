#ifndef _FS_AHCI_H
#define _FS_AHCI_H

/* Optional AHCI disk initialization and its shared IRQ handler. */
void hd_init(void);
void irq_ahci(void);

#endif /* _FS_AHCI_H */
