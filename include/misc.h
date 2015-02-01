#ifndef _MISC_H
#define _MISC_H

/*
 * Mark a function (variable) as being only used at initialization time
 */
#define __tinit __attribute__ ((section (".tinit")))
#define __dinit __attribute__ ((section (".dinit")))

#include <multiboot.h>

void early_print_init(struct multiboot_info *m);
void early_print(const char *fmt, ...);



#endif	/* _MISC_H */
