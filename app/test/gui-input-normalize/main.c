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
	assert(!log.events[0].mouse.absolute);
	assert(log.events[1].type == GUI_INPUT_EVENT_MOUSE_RESET);
	assert(log.events[1].ticks == 21);
	assert(log.events[2].type == GUI_INPUT_EVENT_MOUSE);
	assert(log.events[2].ticks == 21 && log.events[2].mouse.dx == -1);
	assert(log.events[2].mouse.dy == 4 && !log.events[2].mouse.buttons);
}

static void test_absolute_mouse(void)
{
	const struct mario_psaux_event records[] = {
		{ .x = 50000, .y = 40000, .buttons = 7, .ticks = 30,
		  .flags = PSAUX_EVENT_ABSOLUTE | PSAUX_EVENT_OVERFLOW },
	};
	const uint32_t extents[] = { 1, 768, 1024, 1080, 1920 };
	event_log log = { 0 };
	gui_input_event event = { .type = GUI_INPUT_EVENT_MOUSE, .mouse.absolute = 1 };
	uint32_t cursor_x = 123;
	uint32_t cursor_y = 456;

	gui_input_normalize_mouse_records(records, 1, record_event, &log);
	assert(log.count == 2);
	assert(log.events[0].type == GUI_INPUT_EVENT_MOUSE_RESET);
	assert(log.events[0].ticks == 30);
	assert(log.events[1].type == GUI_INPUT_EVENT_MOUSE);
	assert(log.events[1].ticks == 30 && log.events[1].mouse.absolute);
	assert(log.events[1].mouse.x == 50000 && log.events[1].mouse.y == 40000);
	assert(log.events[1].mouse.buttons == 7 && log.events[1].mouse.dz == 0);
	for (size_t index = 0; index < sizeof(extents) / sizeof(extents[0]); index++) {
		uint32_t extent = extents[index];

		for (uint32_t pixel = 0; pixel < extent; pixel++) {
			uint32_t normalized = pixel * 32767 / extent;

			event.mouse.x = normalized * PSAUX_ABSOLUTE_MAX / 32767;
			event.mouse.y = event.mouse.x;
			gui_input_apply_mouse_motion(&event, extent, extent, &cursor_x, &cursor_y);
			assert(cursor_x == pixel && cursor_y == pixel);
		}
	}
	event.mouse.x = PSAUX_ABSOLUTE_MAX;
	event.mouse.y = PSAUX_ABSOLUTE_MAX;
	gui_input_apply_mouse_motion(&event, 1024, 768, &cursor_x, &cursor_y);
	assert(cursor_x == 1023 && cursor_y == 767);
	event.mouse.x = 0;
	event.mouse.y = 0;
	gui_input_apply_mouse_motion(&event, 1024, 768, &cursor_x, &cursor_y);
	assert(cursor_x == 0 && cursor_y == 0);
}

static void test_relative_mouse(void)
{
	gui_input_event event = {
		.type = GUI_INPUT_EVENT_MOUSE,
		.mouse = { .dx = 12, .dy = -7 },
	};
	uint32_t cursor_x = 100;
	uint32_t cursor_y = 200;

	gui_input_apply_mouse_motion(&event, 1024, 768, &cursor_x, &cursor_y);
	assert(cursor_x == 112 && cursor_y == 207);
	event.mouse.dx = -32768;
	event.mouse.dy = 32767;
	gui_input_apply_mouse_motion(&event, 1024, 768, &cursor_x, &cursor_y);
	assert(cursor_x == 0 && cursor_y == 0);
	event.mouse.dx = 32767;
	event.mouse.dy = -32768;
	gui_input_apply_mouse_motion(&event, 1024, 768, &cursor_x, &cursor_y);
	assert(cursor_x == 1023 && cursor_y == 767);
	gui_input_apply_mouse_motion(&event, 0, 0, &cursor_x, &cursor_y);
	assert(cursor_x == 1023 && cursor_y == 767);
}

int main(void)
{
	test_key_fifo();
	test_mouse_fifo();
	test_absolute_mouse();
	test_relative_mouse();
	gui_input_normalize_key_records(NULL, 0, NULL, NULL);
	gui_input_normalize_mouse_records(NULL, 0, NULL, NULL);
	puts("gui-input-normalize: key fifo ok");
	puts("gui-input-normalize: mouse fifo ok");
	puts("gui-input-normalize: absolute coordinates and relative fallback ok");
	return 0;
}
