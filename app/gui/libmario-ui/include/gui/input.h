#ifndef GUI_INPUT_H
#define GUI_INPUT_H

#include <stddef.h>
#include <stdint.h>

#include <mario/input.h>

/* Wire keysyms use Unicode for printable keys and this private-use range otherwise. */
#define GUI_KEYSYM_SPECIAL_BASE 0x00110000U
#define GUI_KEYSYM_ESCAPE (GUI_KEYSYM_SPECIAL_BASE + 0U)
#define GUI_KEYSYM_BACKSPACE (GUI_KEYSYM_SPECIAL_BASE + 1U)
#define GUI_KEYSYM_LEFT (GUI_KEYSYM_SPECIAL_BASE + 2U)
#define GUI_KEYSYM_RIGHT (GUI_KEYSYM_SPECIAL_BASE + 3U)
#define GUI_KEYSYM_UP (GUI_KEYSYM_SPECIAL_BASE + 4U)
#define GUI_KEYSYM_DOWN (GUI_KEYSYM_SPECIAL_BASE + 5U)
#define GUI_KEYSYM_HOME (GUI_KEYSYM_SPECIAL_BASE + 6U)
#define GUI_KEYSYM_END (GUI_KEYSYM_SPECIAL_BASE + 7U)
#define GUI_KEYSYM_PAGE_UP (GUI_KEYSYM_SPECIAL_BASE + 8U)
#define GUI_KEYSYM_PAGE_DOWN (GUI_KEYSYM_SPECIAL_BASE + 9U)
#define GUI_KEYSYM_INSERT (GUI_KEYSYM_SPECIAL_BASE + 10U)
#define GUI_KEYSYM_DELETE (GUI_KEYSYM_SPECIAL_BASE + 11U)
#define GUI_KEYSYM_SHIFT (GUI_KEYSYM_SPECIAL_BASE + 12U)
#define GUI_KEYSYM_CTRL (GUI_KEYSYM_SPECIAL_BASE + 13U)
#define GUI_KEYSYM_ALT (GUI_KEYSYM_SPECIAL_BASE + 14U)
#define GUI_KEYSYM_SUPER (GUI_KEYSYM_SPECIAL_BASE + 15U)

#define GUI_MOD_SHIFT (1U << 0)
#define GUI_MOD_CTRL (1U << 1)
#define GUI_MOD_ALT (1U << 2)
#define GUI_MOD_SUPER (1U << 3)
#define GUI_MOD_CAPS_LOCK (1U << 4)

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
