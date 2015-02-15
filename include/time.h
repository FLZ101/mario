#ifndef _TIME_H
#define _TIME_H

#define HZ 100

void time_init(void);

extern volatile unsigned long jiffies;

#endif	/* _TIME_H */