#ifndef MARIO_NET_ETHERNET_H
#define MARIO_NET_ETHERNET_H

void ethernet_probe(void);
void ethernet_start(void);
int ethernet_stop(void *unused);
int ethernet_poll(void);
void ethernet_flush(void);
void irq_e1000(unsigned int line);
int ethernet_ioctl(unsigned int cmd, unsigned long arg);

#endif
