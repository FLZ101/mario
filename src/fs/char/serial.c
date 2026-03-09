#include <fs/tty/serial.h>
#include <fs/tty/tty.h>
#include <fs/chrdev.h>

#include <io.h>

static struct serial serial_table[NUM_SERIAL];
static struct tty_struct serial_tty_table[NUM_SERIAL];

unsigned char uart_poll_read(unsigned short port)
{
    while ((inb(port + 5) & 1) == 0)
        ;
    return inb(port);
}

void uart_poll_write(unsigned short port, unsigned char c)
{
    while ((inb(port + 5) & 0x20) == 0)
        ;
    outb(port, c);
}

void irq_uart()
{
    for (int i = 0; i < NUM_SERIAL; ++i) {
        struct serial *ser = &serial_table[i];
        struct tty_struct *tty = &serial_tty_table[i];

        if (!tty->count)
            continue;

        unsigned short port = ser->port;

        uint8_t iir = inb(port + 2);
        uint8_t id = iir & 0x0F;

        switch (id) {
        case 0x0c: // "Character Timeout" (FIFO)
        case 0x04: // Received Data Available
            // While LSR says data is ready
            while (inb(port + 5) & 0x01) {
                unsigned char c = inb(COM1 + 0); // Read RBR
                tty_receive_c(tty, c);
            }
            break;
        default:
            break;
        }
    }
}

// Returns 0 if the initialization succeeds
int uart_init(unsigned short port)
{
    outb(port + 1, 0x00); // Disable all interrupts
    outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(port + 1, 0x00); //                  (hi byte)
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set
    outb(port + 1, 0x03); // Enable Rx and Tx holding register empty interrupts
    outb(port + 1, 0x01); // Enable Rx interrupts

    outb(port + 4, 0x1E); // Set in loopback mode, test the serial chip
    outb(port + 0, 0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(port + 0) != 0xAE)
        return 1;

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(port + 4, 0x0F);
    return 0;
}

static void serial_put_char(struct tty_struct *tty, unsigned char c)
{
    dev_t minor = MINOR(tty->dev);
    struct serial *ser = &serial_table[minor - tty->driver->minor];
    uart_poll_write(ser->port, c);
}

struct tty_driver serial_driver = {
    .minor = SERIAL_MINOR_0,
    .n = NUM_SERIAL,
    .tty_table = serial_tty_table,
    .put_char = serial_put_char,
};

void serial_init()
{
    printk("serial device(s):");

    serial_table[0].port = COM1;
    serial_table[1].port = COM2;
    serial_table[2].port = COM3;
    serial_table[3].port = COM4;

    for (int i = 0; i < NUM_SERIAL; ++i) {
        struct serial *ser = &serial_table[i];
        struct tty_struct *tty = &serial_tty_table[i];

        tty->dev = MKDEV(TTY_MAJOR, SERIAL_MINOR_0 + i);
        tty->driver = &serial_driver;
        tty->pgrp = 0;
        tty->session = 0;
        ring_buffer_init(&tty->read_buf);

        init_wait_queue(&tty->wait_read);

        tty->termios = default_termios;
        tty->winsize.ws_col = 80;
        tty->winsize.ws_row = 24;
        tty->count = 0;
        tty->initialized = 0;

        if (uart_init(ser->port))
            continue;
        tty->initialized = 1;

        printk(" /dev/ttyS%d", i);
    }
    printk("\n");

    register_tty_driver(&serial_driver);
}
