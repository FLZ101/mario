#ifndef _UAPI_MARIO_INPUT_H
#define _UAPI_MARIO_INPUT_H

/*
 * Stable userspace records for Mario's graphics keyboard and PS/2 mouse
 * devices.  They are fixed-width Mario ABIs, not Linux input-event layouts.
 */
#ifdef __KERNEL__
#include <types.h>
#else
#include <stdint.h>
#endif

/* The record follows one or more dropped older keyboard records. */
#define GUI_KEY_EVENT_OVERFLOW 0x01

struct mario_gui_key_event {
	uint16_t scancode; /* Set-1 code; extended codes set bit 8. */
	uint8_t pressed;   /* 1 for make, 0 for break. */
	uint8_t flags;
	uint32_t ticks;    /* Monotonic kernel tick captured at IRQ time. */
};

/* The record follows a dropped or discarded older mouse packet. */
#define PSAUX_EVENT_OVERFLOW 0x01
#define PSAUX_EVENT_ABSOLUTE 0x02
#define PSAUX_ABSOLUTE_MAX 0xffffU

struct mario_psaux_event {
	union {
		int16_t dx;
		uint16_t x;
	};
	union {
		int16_t dy;
		uint16_t y;
	};
	int8_t dz;      /* Vertical wheel steps: negative up, positive down. */
	uint8_t buttons; /* Bit 0 left, bit 1 right, bit 2 middle. */
	uint8_t flags;
	uint8_t reserved;
	uint32_t ticks; /* Monotonic kernel tick captured at IRQ time. */
};

/* Catch accidental ABI changes in both kernel and userspace builds. */
_Static_assert(sizeof(struct mario_gui_key_event) == 8,
	       "mario_gui_key_event ABI size");
_Static_assert(sizeof(struct mario_psaux_event) == 12,
	       "mario_psaux_event ABI size");

#endif /* _UAPI_MARIO_INPUT_H */
