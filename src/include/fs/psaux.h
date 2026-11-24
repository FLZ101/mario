#ifndef _FS_PSAUX_H
#define _FS_PSAUX_H

#include <types.h>

/* The PS/2 controller reports completed auxiliary bytes to this FIFO. */
void psaux_init(void);
void psaux_controller_ready(uint8_t device_id);
void psaux_controller_failed(void);
void psaux_receive_byte(uint8_t byte);
void irq_PS2_aux(void);

#endif /* _FS_PSAUX_H */
