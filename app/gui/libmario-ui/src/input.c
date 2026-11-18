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
				.absolute = !!(records[index].flags & PSAUX_EVENT_ABSOLUTE),
			},
		};

		/* Overflow invalidates button state before this packet is handled. */
		if (records[index].flags & PSAUX_EVENT_OVERFLOW)
			gui_input_emit_mouse_reset(emit, context, event.ticks);
		emit(context, &event);
	}
}

void gui_input_apply_mouse_motion(const gui_input_event *event, uint32_t width,
				  uint32_t height, uint32_t *cursor_x,
				  uint32_t *cursor_y)
{
	int64_t next_x;
	int64_t next_y;

	if (event == NULL || cursor_x == NULL || cursor_y == NULL || !width || !height)
		return;
	if (event->mouse.absolute) {
		next_x = ((uint64_t)event->mouse.x * width + PSAUX_ABSOLUTE_MAX / 2) /
			PSAUX_ABSOLUTE_MAX;
		next_y = ((uint64_t)event->mouse.y * height + PSAUX_ABSOLUTE_MAX / 2) /
			PSAUX_ABSOLUTE_MAX;
	} else {
		next_x = (int64_t)*cursor_x + event->mouse.dx;
		next_y = (int64_t)*cursor_y - event->mouse.dy;
	}
	if (next_x < 0)
		next_x = 0;
	else if ((uint64_t)next_x >= width)
		next_x = width - 1;
	if (next_y < 0)
		next_y = 0;
	else if ((uint64_t)next_y >= height)
		next_y = height - 1;
	*cursor_x = next_x;
	*cursor_y = next_y;
}
