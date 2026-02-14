#include <io.h>
#include <types.h>
#include <misc.h>

/*
 * https://wiki.osdev.org/Serial_Ports
 */

#define COM1 0x3F8  // IRQ4
#define COM2 0x2F8  // IRQ3
#define COM3 0x3E8  // IRQ4
#define COM4 0x2E8  // IRQ3

#define MAX_UART    4

char poll_read(unsigned short port)
{
    while ((inb(port + 5) & 1) == 0)
        ;
    return inb(port);
}

void poll_write(unsigned short port, char c)
{
    while ((inb(port + 5) & 0x20) == 0)
        ;
    outb(port, c);
}

void irq_uart(unsigned short port)
{
    uint8_t iir = inb(port + 2);
    uint8_t id = iir & 0x0F;

    switch (id) {
    case 0x0c: // "Character Timeout" (FIFO)
    case 0x04: // Received Data Available
        // While LSR says data is ready
        while (inb(port + 5) & 0x01) {
            char c = inb(COM1 + 0); // Read RBR
            printk("%c", c);
        }
        break;
    default:
        break;
    }
}

// Initialize UART (115200 baud, 8N1)
void uart_init(unsigned short port)
{
    outb(port + 1, 0x00); // Disable all interrupts
    outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(port + 1, 0x00); //                  (hi byte)
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set
    // outb(port + 1, 0x03); // Enable Rx and Tx holding register empty interrupts
    outb(port + 1, 0x01); // Enable Rx interrupts

    outb(port + 4, 0x1E); // Set in loopback mode, test the serial chip
    outb(port + 0, 0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(port + 0) != 0xAE)
    {
        printk("not ok\n");
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(port + 4, 0x0F);

    poll_write(port, '@');
}
