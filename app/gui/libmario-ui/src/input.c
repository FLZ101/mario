#include <gui/input.h>

static void gui_input_emit_key_reset(gui_input_emit_fn emit, void *context,
				     uint32_t ticks)
{
	gui_input_event event = {
		.type = GUI_INPUT_EVENT_KEY_RESET,
		.ticks = ticks,
	};

	emit(context, &event);
}

static void gui_input_emit_mouse_reset(gui_input_emit_fn emit, void *context,
				       uint32_t ticks)
{
	gui_input_event event = {
		.type = GUI_INPUT_EVENT_MOUSE_RESET,
		.ticks = ticks,
	};

	emit(context, &event);
}

void gui_input_normalize_key_records(const struct mario_gui_key_event *records,
				     size_t count, gui_input_emit_fn emit,
				     void *context)
{
	if (records == NULL || emit == NULL)
		return;
	for (size_t index = 0; index < count; index++) {
		gui_input_event event = {
			.type = GUI_INPUT_EVENT_KEY,
			.ticks = records[index].ticks,
			.key = {
				.scancode = records[index].scancode,
				.pressed = records[index].pressed,
			},
		};

		/* A reset must precede its marked record, never later input. */
		if (records[index].flags & GUI_KEY_EVENT_OVERFLOW)
			gui_input_emit_key_reset(emit, context, event.ticks);
		emit(context, &event);
	}
}

void gui_input_normalize_mouse_records(const struct mario_psaux_event *records,
				       size_t count, gui_input_emit_fn emit,
				       void *context)
{
	if (records == NULL || emit == NULL)
		return;
	for (size_t index = 0; index < count; index++) {
		gui_input_event event = {
			.type = GUI_INPUT_EVENT_MOUSE,
			.ticks = records[index].ticks,
			.mouse = {
				.dx = records[index].dx,
				.dy = records[index].dy,
				.dz = records[index].dz,
				.buttons = records[index].buttons,
			},
		};

		/* Overflow invalidates button state before this packet is handled. */
		if (records[index].flags & PSAUX_EVENT_OVERFLOW)
			gui_input_emit_mouse_reset(emit, context, event.ticks);
		emit(context, &event);
	}
}
