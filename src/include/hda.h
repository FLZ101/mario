#ifndef _HDA_H
#define _HDA_H

#include <types.h>

#define HDA_PCM_FORMAT 0x0011
#define HDA_STREAM_TAG 1
#define HDA_PERIOD_FRAMES 960
#define HDA_PERIOD_BYTES (HDA_PERIOD_FRAMES * 4)
#define HDA_PERIODS 4
#define HDA_BUFFER_BYTES (HDA_PERIODS * HDA_PERIOD_BYTES)
#define HDA_WALL_TICKS_PER_MS 24000U

struct multiboot_info;

void hda_boot_options(const struct multiboot_info *boot);
void hda_init(void);
int hda_verb(unsigned int codec, unsigned int node, uint32_t verb,
	     uint32_t *response);
int hda_configure_codec(unsigned int codec);
int hda_pcm_present(void);
int hda_pcm_defer_first_poll(void);
int hda_pcm_start(void);
int hda_pcm_stop(void);
int hda_pcm_position(void);
uint32_t hda_pcm_clock(void);
int16_t *hda_pcm_period(unsigned int index);

void pcm_init(void);
void pcm_service(void);

#endif
