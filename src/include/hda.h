#ifndef _HDA_H
#define _HDA_H

struct multiboot_info;

void hda_boot_options(const struct multiboot_info *boot);
void hda_init(void);

#endif
