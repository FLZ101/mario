#ifndef GUI_INPUT_H
#define GUI_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include <mario/input.h>

typedef enum {
	GUI_INPUT_EVENT_KEY,
	GUI_INPUT_EVENT_MOUSE,
	GUI_INPUT_EVENT_KEY_RESET,
	GUI_INPUT_EVENT_MOUSE_RESET,
} gui_input_event_type;

typedef struct {
	gui_input_event_type type;
	uint32_t ticks;
	union {
		struct {
			uint16_t scancode;
			uint8_t pressed;
		} key;
		struct {
			int16_t dx;
			int16_t dy;
			int8_t dz;
			uint8_t buttons;
		} mouse;
	};
} gui_input_event;

typedef void (*gui_input_emit_fn)(void *context, const gui_input_event *event);

void gui_input_normalize_key_records(const struct mario_gui_key_event *records,
				     size_t count, gui_input_emit_fn emit,
				     void *context);
void gui_input_normalize_mouse_records(const struct mario_psaux_event *records,
				       size_t count, gui_input_emit_fn emit,
				       void *context);

#endif
