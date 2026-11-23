#ifndef _UAPI_MARIO_PCM_H
#define _UAPI_MARIO_PCM_H

#ifdef __KERNEL__
#include <types.h>
#else
#include <stdint.h>
#endif

#define MARIO_PCM_ABI_VERSION 1
#define MARIO_PCM_RATE 48000
#define MARIO_PCM_FRAME_BYTES 4

#define MARIO_PCM_START  0x5000
#define MARIO_PCM_PAUSE  0x5001
#define MARIO_PCM_RESUME 0x5002
#define MARIO_PCM_RESET  0x5003
#define MARIO_PCM_STATUS 0x5004
#define MARIO_PCM_DRAIN  0x5005

enum mario_pcm_state {
	MARIO_PCM_STOPPED,
	MARIO_PCM_RUNNING,
	MARIO_PCM_PAUSING,
	MARIO_PCM_PAUSED,
	MARIO_PCM_DRAINING,
	MARIO_PCM_UNDERRUN,
	MARIO_PCM_FAILED,
};

struct mario_pcm_status {
	uint32_t version;
	uint32_t state;
	uint32_t capacity_frames;
	uint32_t writable_frames;
	uint32_t pending_frames;
	uint32_t underruns;
	uint64_t consumed_frames;
};

_Static_assert(sizeof(struct mario_pcm_status) == 32, "pcm status ABI size");

#endif
