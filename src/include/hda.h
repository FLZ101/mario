#ifndef _HDA_H
#define _HDA_H

#include <types.h>

#define HDA_PCM_FORMAT 0x0011
#define HDA_STREAM_TAG 1

struct multiboot_info;

void hda_boot_options(const struct multiboot_info *boot);
void hda_init(void);
int hda_verb(unsigned int codec, unsigned int node, uint32_t verb,
	     uint32_t *response);
int hda_configure_codec(unsigned int codec);

#endif
