#ifndef _SERIAL_H
#define _SERIAL_H

/*
 * https://wiki.osdev.org/Serial_Ports
 */

#define COM1 0x3F8  // IRQ4
#define COM2 0x2F8  // IRQ3
#define COM3 0x3E8  // IRQ4
#define COM4 0x2E8  // IRQ3

struct serial {
    unsigned short port;
};

#endif /* _SERIAL_H */
