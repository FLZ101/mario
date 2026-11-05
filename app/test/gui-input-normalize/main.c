#include <assert.h>
#include <stdio.h>

#include <gui/input.h>

typedef struct {
	gui_input_event events[8];
	int count;
} event_log;

static void record_event(void *context, const gui_input_event *event)
{
	event_log *log = context;

	assert(log->count < (int)(sizeof(log->events) / sizeof(log->events[0])));
	log->events[log->count++] = *event;
}

static void test_key_fifo(void)
{
	const struct mario_gui_key_event records[] = {
		{ .scancode = 0x1e, .pressed = 1, .ticks = 10 },
		{ .scancode = 0x9e, .pressed = 0,
		  .flags = GUI_KEY_EVENT_OVERFLOW, .ticks = 11 },
		{ .scancode = 0x30, .pressed = 1, .ticks = 12 },
	};
	event_log log = { 0 };

	gui_input_normalize_key_records(records, 3, record_event, &log);
	assert(log.count == 4);
	assert(log.events[0].type == GUI_INPUT_EVENT_KEY);
	assert(log.events[0].ticks == 10 && log.events[0].key.scancode == 0x1e);
	assert(log.events[1].type == GUI_INPUT_EVENT_KEY_RESET);
	assert(log.events[1].ticks == 11);
	assert(log.events[2].type == GUI_INPUT_EVENT_KEY);
	assert(log.events[2].ticks == 11 && !log.events[2].key.pressed);
	assert(log.events[3].type == GUI_INPUT_EVENT_KEY);
	assert(log.events[3].ticks == 12 && log.events[3].key.scancode == 0x30);
}

static void test_mouse_fifo(void)
{
	const struct mario_psaux_event records[] = {
		{ .dx = 3, .dy = -2, .buttons = 1, .ticks = 20 },
		{ .dx = -1, .dy = 4, .dz = 0, .buttons = 0,
		  .flags = PSAUX_EVENT_OVERFLOW, .ticks = 21 },
	};
	event_log log = { 0 };

	gui_input_normalize_mouse_records(records, 2, record_event, &log);
	assert(log.count == 3);
	assert(log.events[0].type == GUI_INPUT_EVENT_MOUSE);
	assert(log.events[0].ticks == 20 && log.events[0].mouse.dx == 3);
	assert(log.events[1].type == GUI_INPUT_EVENT_MOUSE_RESET);
	assert(log.events[1].ticks == 21);
	assert(log.events[2].type == GUI_INPUT_EVENT_MOUSE);
	assert(log.events[2].ticks == 21 && log.events[2].mouse.dx == -1);
	assert(log.events[2].mouse.dy == 4 && !log.events[2].mouse.buttons);
}

int main(void)
{
	test_key_fifo();
	test_mouse_fifo();
	gui_input_normalize_key_records(NULL, 0, NULL, NULL);
	gui_input_normalize_mouse_records(NULL, 0, NULL, NULL);
	puts("gui-input-normalize: key fifo ok");
	puts("gui-input-normalize: mouse fifo ok");
	return 0;
}
