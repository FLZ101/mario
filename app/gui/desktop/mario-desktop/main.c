#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <gui/image.h>
#include <gui/appearance.h>
#include <gui/font.h>
#include <gui/input.h>
#include <gui/protocol.h>
#include <app/gui-limits.h>

#define WM_FD 3
#define WM_SOCKET_PATH "/tmp/mario-wm.sock"
#define SURFACE_PIXELS_PER_MESSAGE 1000U
#define DESKTOP_BACKGROUND 0x001f2933U
#define DESKTOP_WALLPAPER_PATH GUI_APPEARANCE_WALLPAPER_DEFAULT
#define DESKTOP_PANEL 0x00111827U
#define DESKTOP_LAUNCHER 0x002563ebU
#define DESKTOP_TASK_ACTIVE 0x002563ebU
#define DESKTOP_TASK_INACTIVE 0x00475569U
#define DESKTOP_TEXT 0x00f8fafcU
#define DESKTOP_LAUNCH_ERROR 0x00ef4444U
#define DESKTOP_PANEL_HEIGHT 28U
#define DESKTOP_TASKBAR_FIRST_X 64U
#define DESKTOP_TASKBAR_BUTTON_WIDTH 96U
#define DESKTOP_TASKBAR_BUTTON_STRIDE 100U
#define DESKTOP_TASKBAR_BUTTON_HEIGHT 20U
#define DESKTOP_TASKBAR_CONTROL_WIDTH 20U
#define DESKTOP_TASKBAR_CONTROL_STRIDE 24U
#define DESKTOP_MENU_X 8U
#define DESKTOP_MENU_WIDTH 120U
#define DESKTOP_MENU_ITEM_HEIGHT 20U
#define DESKTOP_MENU_ITEM_COUNT 1U
#define DIAGNOSTIC_WELCOME 0U
#define DIAGNOSTIC_DISPLAY 1U
#define DIAGNOSTIC_COUNT 2U

typedef enum {
	DESKTOP_MENU_TERMINAL,
} desktop_menu_entry;

typedef struct {
	int fd;
	uint32_t window_id;
	uint64_t configure_serial;
	uint64_t frame_serial;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	int32_t x;
	int32_t y;
	int redraw;
	app_gui_surface_flow surface_flow;
} diagnostic_client;

typedef struct {
	const char *title;
	uint32_t width;
	uint32_t height;
	uint32_t color;
	int32_t x;
	int32_t y;
} diagnostic_definition;

static const diagnostic_definition diagnostic_definitions[DIAGNOSTIC_COUNT] = {
	[DIAGNOSTIC_WELCOME] = {
		.title = "Welcome", .width = 360, .height = 220,
		.color = 0x001d4ed8U, .x = 96, .y = 96,
	},
	[DIAGNOSTIC_DISPLAY] = {
		.title = "Display", .width = 320, .height = 192,
		.color = 0x000f766eU, .x = 480, .y = 220,
	},
};

typedef struct desktop_window {
	struct desktop_window *next;
	uint32_t window_id;
	uint32_t focused;
	uint32_t order;
} desktop_window;

typedef struct {
	uint32_t window_id;
	uint32_t focused;
	char label[11];
} desktop_button;

typedef struct {
	desktop_button *buttons;
	size_t capacity;
	size_t count;
	size_t page;
	size_t pages;
	uint32_t first_x;
	int paged;
} desktop_taskbar;

typedef struct {
	uint32_t window_id;
	uint64_t configure_serial;
	uint32_t width;
	uint32_t height;
	uint64_t frame_serial;
	uint8_t mouse_buttons;
	uint8_t menu_open;
	uint8_t launch_failed;
	uint32_t last_focused_window_id;
	uint32_t menu_restore_window_id;
	uint32_t next_request_id;
	uint32_t pending_request_id;
	int redraw;
	app_gui_surface_flow surface_flow;
	desktop_window *windows;
	size_t record_bytes;
	size_t window_count;
	size_t taskbar_page;
	desktop_button *taskbar_storage;
	desktop_taskbar painted_taskbar;
	desktop_taskbar displayed_taskbar;
	gui_surface *wallpaper;
	uint32_t *wallpaper_map;
	uint32_t wallpaper_width;
	uint32_t wallpaper_height;
	gui_appearance appearance;
	gui_font *font;
	gui_font_metrics font_metrics;
	gui_surface *text_surface;
	uint32_t panel_height;
	uint32_t button_height;
	uint32_t launcher_width;
	uint32_t clock_width;
	uint32_t menu_width;
	uint32_t taskbar_width;
} desktop_state;

/* Keep the largest wire frame out of the small initial userspace stack. */
static uint8_t desktop_frame[GUI_PROTOCOL_HEADER_SIZE +
	sizeof(gui_protocol_wire_surface_data) +
	SURFACE_PIXELS_PER_MESSAGE * sizeof(uint32_t)];

static void read_full(int fd, void *buffer, size_t byte_count)
{
	uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = read(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			fprintf(stderr, "mario-desktop: read failed on fd %d: count %ld, errno %d\n",
				fd, (long)count, errno);
			_exit(1);
		}
		bytes += count;
		byte_count -= count;
	}
}

static int write_full(int fd, const void *buffer, size_t byte_count)
{
	const uint8_t *bytes = buffer;

	while (byte_count != 0) {
		ssize_t count = write(fd, bytes, byte_count);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			return -1;
		bytes += count;
		byte_count -= count;
	}
	return 0;
}

static void put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned int index = 0; index < 4; index++)
		bytes[index] = value >> (index * 8);
}

static void put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = value;
	bytes[1] = value >> 8;
}

static void put64(uint8_t *bytes, uint64_t value)
{
	for (unsigned int index = 0; index < 8; index++)
		bytes[index] = value >> (index * 8);
}

static uint32_t get32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
		(uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t get64(const uint8_t *bytes)
{
	uint64_t value = 0;

	for (unsigned int index = 0; index < 8; index++)
		value |= (uint64_t)bytes[index] << (index * 8);
	return value;
}

static int glyph_pixel(char character, unsigned int column, unsigned int row)
{
	static const uint8_t glyphs[][5] = {
		/* - */ { 0x00, 0x00, 0x07, 0x00, 0x00 },
		/* : */ { 0x00, 0x02, 0x00, 0x02, 0x00 },
		/* E */ { 0x07, 0x04, 0x06, 0x04, 0x07 },
		/* M */ { 0x05, 0x07, 0x07, 0x05, 0x05 },
		/* N */ { 0x05, 0x07, 0x07, 0x07, 0x05 },
		/* U */ { 0x05, 0x05, 0x05, 0x05, 0x07 },
		{ 0x07, 0x05, 0x05, 0x05, 0x07 },
		{ 0x02, 0x06, 0x02, 0x02, 0x07 },
		{ 0x07, 0x01, 0x07, 0x04, 0x07 },
		{ 0x07, 0x01, 0x07, 0x01, 0x07 },
		{ 0x05, 0x05, 0x07, 0x01, 0x01 },
		{ 0x07, 0x04, 0x07, 0x01, 0x07 },
		{ 0x07, 0x04, 0x07, 0x05, 0x07 },
		{ 0x07, 0x01, 0x01, 0x01, 0x01 },
		{ 0x07, 0x05, 0x07, 0x05, 0x07 },
		{ 0x07, 0x05, 0x07, 0x01, 0x07 },
		/* A */ { 0x02, 0x05, 0x07, 0x05, 0x05 },
		/* F */ { 0x07, 0x04, 0x07, 0x04, 0x04 },
		/* G */ { 0x07, 0x04, 0x05, 0x05, 0x03 },
		/* I */ { 0x07, 0x02, 0x02, 0x02, 0x07 },
		/* L */ { 0x04, 0x04, 0x04, 0x04, 0x07 },
		/* R */ { 0x07, 0x05, 0x06, 0x05, 0x05 },
		/* S */ { 0x07, 0x04, 0x07, 0x01, 0x07 },
		/* T */ { 0x07, 0x02, 0x02, 0x02, 0x02 },
	};
	const uint8_t *glyph;

	if (column >= 3 || row >= 5)
		return 0;
	switch (character) {
	case '-': glyph = glyphs[0]; break;
	case ':': glyph = glyphs[1]; break;
	case 'U': glyph = glyphs[5]; break;
	case 'a':
	case 'A': glyph = glyphs[16]; break;
	case 'e':
	case 'E': glyph = glyphs[2]; break;
	case 'f':
	case 'F': glyph = glyphs[17]; break;
	case 'g':
	case 'G': glyph = glyphs[18]; break;
	case 'i':
	case 'I': glyph = glyphs[19]; break;
	case 'l':
	case 'L': glyph = glyphs[20]; break;
	case 'm':
	case 'M': glyph = glyphs[3]; break;
	case 'n':
	case 'N': glyph = glyphs[4]; break;
	case 'r':
	case 'R': glyph = glyphs[21]; break;
	case 's':
	case 'S': glyph = glyphs[22]; break;
	case 't':
	case 'T': glyph = glyphs[23]; break;
	default:
		if (character < '0' || character > '9')
			return 0;
		glyph = glyphs[6 + character - '0'];
		break;
	}
	return glyph[row] & (1U << (2 - column));
}

static int text_pixel(const char *text, uint32_t x, uint32_t y,
		      uint32_t origin_x, uint32_t origin_y)
{
	uint32_t character_index;

	if (x < origin_x || y < origin_y || y >= origin_y + 10)
		return 0;
	character_index = (x - origin_x) / 8;
	if (character_index >= strlen(text))
		return 0;
	return glyph_pixel(text[character_index],
		((x - origin_x) % 8) / 2, (y - origin_y) / 2);
}

static desktop_window *desktop_window_by_id(desktop_state *state,
					     uint32_t window_id)
{
	for (desktop_window *window = state->windows; window != NULL; window = window->next)
		if (window->window_id == window_id)
			return window;
	return NULL;
}

static uint32_t desktop_panel_height(const desktop_state *state)
{
	return state->panel_height ? state->panel_height : DESKTOP_PANEL_HEIGHT;
}

static uint32_t desktop_button_height(const desktop_state *state)
{
	return state->button_height ? state->button_height : DESKTOP_TASKBAR_BUTTON_HEIGHT;
}

static uint32_t desktop_launcher_width(const desktop_state *state)
{
	return state->launcher_width ? state->launcher_width : 48;
}

static uint32_t desktop_clock_width(const desktop_state *state)
{
	return state->clock_width ? state->clock_width : 40;
}

static uint32_t desktop_menu_width(const desktop_state *state)
{
	return state->menu_width ? state->menu_width : DESKTOP_MENU_WIDTH;
}

static uint32_t desktop_taskbar_width(const desktop_state *state)
{
	return state->taskbar_width ? state->taskbar_width : DESKTOP_TASKBAR_BUTTON_WIDTH;
}

static uint32_t desktop_taskbar_first_x(const desktop_state *state)
{
	return desktop_launcher_width(state) + 16;
}

static uint32_t desktop_taskbar_stride(const desktop_state *state)
{
	return desktop_taskbar_width(state) + 4;
}

static uint32_t desktop_panel_y(const desktop_state *state)
{
	return state->height > desktop_panel_height(state) ?
		state->height - desktop_panel_height(state) : 0;
}

static uint32_t desktop_text_width(desktop_state *state, const char *text, uint32_t minimum)
{
	gui_text_layout layout;

	if (!gui_text_measure(text, &state->font_metrics, gui_font_lookup, state->font, &layout) ||
	    layout.width < 0 || layout.width > 512 || (uint32_t)layout.width + 16 < minimum)
		return minimum;
	return (uint32_t)layout.width + 16;
}

static void desktop_setup_font(desktop_state *state)
{
	gui_font_metrics *metrics = &state->font_metrics;
	uint32_t height;

	if (state->appearance.font_path[0] == '\0')
		return;
	state->font = gui_font_load(state->appearance.font_path, state->appearance.font_size);
	if (state->font == NULL || !gui_font_get_metrics(state->font, metrics) ||
	    metrics->ascent <= 0 || metrics->ascent > 128 || metrics->descent > 0 ||
	    metrics->descent < -128 || metrics->line_gap < 0 || metrics->line_gap > 128) {
		fprintf(stderr, "mario-desktop: font unavailable: %s; using bitmap font\n",
			state->appearance.font_path);
		gui_font_destroy(state->font);
		state->font = NULL;
		return;
	}
	height = metrics->ascent - metrics->descent + metrics->line_gap;
	state->button_height = height + 4 > DESKTOP_TASKBAR_BUTTON_HEIGHT ?
		height + 4 : DESKTOP_TASKBAR_BUTTON_HEIGHT;
	state->panel_height = state->button_height + 8;
	state->launcher_width = desktop_text_width(state, "MENU", 48);
	state->clock_width = desktop_text_width(state, "--:--", 40);
	state->menu_width = desktop_text_width(state, "Terminal", DESKTOP_MENU_WIDTH);
	state->taskbar_width = desktop_text_width(state, "4294967295", DESKTOP_TASKBAR_BUTTON_WIDTH);
}

static int desktop_bitmap_text(const desktop_state *state, const char *text,
		uint32_t x, uint32_t y, uint32_t origin_x, uint32_t origin_y)
{
	return (state->font == NULL || state->text_surface == NULL) &&
		text_pixel(text, x, y, origin_x, origin_y);
}

static size_t desktop_taskbar_capacity(const desktop_state *state, int paged)
{
	uint32_t clock_x = state->width > desktop_clock_width(state) ?
		state->width - desktop_clock_width(state) : 0;
	uint32_t reserved = desktop_taskbar_first_x(state) +
		(paged ? 2 * DESKTOP_TASKBAR_CONTROL_STRIDE : 0);

	return clock_x < reserved + desktop_taskbar_width(state) ? 0 :
		(clock_x - reserved - desktop_taskbar_width(state)) /
		desktop_taskbar_stride(state) + 1;
}

static int desktop_prepare_taskbar(desktop_state *state)
{
	desktop_taskbar *taskbar = &state->painted_taskbar;
	size_t maximum = desktop_taskbar_capacity(state, 0);
	const desktop_window *window = state->windows;
	size_t offset;

	if (maximum != 0 && state->taskbar_storage == NULL) {
		size_t bytes;

		if (maximum > SIZE_MAX / (2 * sizeof(desktop_button)))
			return -1;
		bytes = 2 * maximum * sizeof(desktop_button);
		if (state->record_bytes > APP_GUI_DESKTOP_RECORD_BYTES ||
		    bytes > APP_GUI_DESKTOP_RECORD_BYTES - state->record_bytes ||
		    (state->taskbar_storage = calloc(1, bytes)) == NULL)
			return -1;
		state->record_bytes += bytes;
		taskbar->buttons = state->taskbar_storage;
		state->displayed_taskbar.buttons = state->taskbar_storage + maximum;
	}
	taskbar->paged = state->window_count > maximum;
	taskbar->capacity = desktop_taskbar_capacity(state, taskbar->paged);
	taskbar->pages = taskbar->capacity == 0 || state->window_count == 0 ? 1 :
		(state->window_count - 1) / taskbar->capacity + 1;
	if (state->taskbar_page >= taskbar->pages)
		state->taskbar_page = taskbar->pages - 1;
	taskbar->page = state->taskbar_page;
	taskbar->first_x = desktop_taskbar_first_x(state) +
		(taskbar->paged ? DESKTOP_TASKBAR_CONTROL_STRIDE : 0);
	taskbar->count = 0;
	offset = taskbar->page * taskbar->capacity;
	while (window != NULL && offset != 0) {
		window = window->next;
		offset--;
	}
	while (window != NULL && taskbar->count < taskbar->capacity) {
		desktop_button *button = &taskbar->buttons[taskbar->count++];

		*button = (desktop_button) {
			.window_id = window->window_id, .focused = window->focused };
		snprintf(button->label, sizeof(button->label), "%u", window->window_id);
		window = window->next;
	}
	return 0;
}

static void desktop_commit_taskbar(desktop_state *state)
{
	desktop_button *buttons = state->displayed_taskbar.buttons;

	state->displayed_taskbar = state->painted_taskbar;
	state->displayed_taskbar.buttons = buttons;
	if (state->painted_taskbar.count != 0)
		memcpy(buttons, state->painted_taskbar.buttons,
			state->painted_taskbar.count * sizeof(*buttons));
}

static const desktop_button *desktop_taskbar_button(const desktop_state *state,
		const desktop_taskbar *taskbar, uint32_t pointer_x, uint32_t pointer_y)
{
	uint32_t panel_y = desktop_panel_y(state);
	size_t slot;

	if (pointer_y < panel_y + 4 || pointer_y >= panel_y + 4 + desktop_button_height(state) ||
	    pointer_x < taskbar->first_x)
		return NULL;
	slot = (pointer_x - taskbar->first_x) / desktop_taskbar_stride(state);
	if (slot >= taskbar->count ||
	    (pointer_x - taskbar->first_x) % desktop_taskbar_stride(state) >=
	    desktop_taskbar_width(state))
		return NULL;
	return &taskbar->buttons[slot];
}

static const desktop_window *desktop_taskbar_window(desktop_state *state,
		uint32_t pointer_x, uint32_t pointer_y)
{
	const desktop_button *button = desktop_taskbar_button(state,
		&state->displayed_taskbar, pointer_x, pointer_y);

	return button == NULL ? NULL : desktop_window_by_id(state, button->window_id);
}

static int desktop_taskbar_control(const desktop_state *state,
		const desktop_taskbar *taskbar, uint32_t pointer_x, uint32_t pointer_y)
{
	uint32_t panel_y = desktop_panel_y(state);
	uint32_t next_x = state->width > desktop_clock_width(state) + DESKTOP_TASKBAR_CONTROL_STRIDE ?
		state->width - desktop_clock_width(state) - DESKTOP_TASKBAR_CONTROL_STRIDE : 0;

	if (!taskbar->paged || taskbar->capacity == 0 || pointer_y < panel_y + 4 ||
	    pointer_y >= panel_y + 4 + desktop_button_height(state))
		return 0;
	if (pointer_x >= desktop_taskbar_first_x(state) &&
	    pointer_x < desktop_taskbar_first_x(state) + DESKTOP_TASKBAR_CONTROL_WIDTH)
		return -1;
	if (pointer_x >= next_x && pointer_x < next_x + DESKTOP_TASKBAR_CONTROL_WIDTH)
		return 1;
	return 0;
}

static void desktop_reveal_window(desktop_state *state, const desktop_window *target)
{
	size_t capacity = desktop_taskbar_capacity(state,
		state->window_count > desktop_taskbar_capacity(state, 0));
	size_t position = 0;

	for (const desktop_window *window = state->windows; window != NULL; window = window->next) {
		if (window == target) {
			state->taskbar_page = capacity == 0 ? 0 : position / capacity;
			return;
		}
		position++;
	}
}

static int desktop_read_wallpaper(const char *path, gui_surface **image)
{
	static const uint8_t png_signature[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	struct stat status;
	uint8_t *contents = NULL;
	size_t byte_count;
	size_t offset = 0;
	int result = -1;
	int saved_errno;
	int fd;

	if (path == NULL || image == NULL) {
		errno = EINVAL;
		return -1;
	}
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (fstat(fd, &status) < 0)
		goto done;
	if (!S_ISREG(status.st_mode) || status.st_size <= 0) {
		errno = EINVAL;
		goto done;
	}
	if ((uintmax_t)status.st_size > GUI_IMAGE_MAX_FILE_BYTES) {
		errno = EFBIG;
		goto done;
	}
	byte_count = (size_t)status.st_size;
	contents = malloc(byte_count);
	if (contents == NULL)
		goto done;
	while (offset < byte_count) {
		ssize_t count = read(fd, contents + offset, byte_count - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			if (count == 0)
				errno = EIO;
			goto done;
		}
		offset += (size_t)count;
	}
	result = byte_count >= sizeof(png_signature) &&
		memcmp(contents, png_signature, sizeof(png_signature)) == 0 ?
		gui_png_decode(contents, byte_count, image) :
		gui_bmp_decode(contents, byte_count, image);
done:
	saved_errno = errno;
	close(fd);
	free(contents);
	errno = saved_errno;
	return result;
}

static void desktop_load_wallpaper(desktop_state *state)
{
	const char *path = getenv("MARIO_WALLPAPER");
	int configured = path != NULL;

	if (path == NULL) {
		path = state->appearance.font_size ? state->appearance.wallpaper_path : DESKTOP_WALLPAPER_PATH;
		configured = strcmp(path, DESKTOP_WALLPAPER_PATH) != 0;
	}
	if (*path == '\0')
		return;
	if (desktop_read_wallpaper(path, &state->wallpaper) < 0) {
		if (configured || errno != ENOENT)
			fprintf(stderr, "mario-desktop: wallpaper unavailable: %s: %s; using solid background\n",
				path, strerror(errno));
		return;
	}
	fprintf(stderr, "mario-desktop: wallpaper loaded: %s (%dx%d)\n", path,
		state->wallpaper->width, state->wallpaper->height);
}

static void desktop_prepare_wallpaper(desktop_state *state)
{
	uint64_t map_count = (uint64_t)state->width + state->height;
	uint32_t source_width;
	uint32_t source_height;
	uint64_t margin;
	int width_dominant;

	if (state->wallpaper == NULL || (state->wallpaper_map != NULL &&
	    state->wallpaper_width == state->width && state->wallpaper_height == state->height))
		return;
	free(state->wallpaper_map);
	state->wallpaper_map = NULL;
	state->wallpaper_width = state->wallpaper_height = 0;
	if (state->width == 0 || state->height == 0 || desktop_panel_y(state) == 0)
		return;
	if (map_count > SIZE_MAX / sizeof(*state->wallpaper_map) ||
	    (state->wallpaper_map = malloc((size_t)map_count * sizeof(*state->wallpaper_map))) == NULL) {
		fprintf(stderr, "mario-desktop: wallpaper scaling allocation failed; using solid background\n");
		return;
	}
	source_width = (uint32_t)state->wallpaper->width;
	source_height = (uint32_t)state->wallpaper->height;
	width_dominant = (uint64_t)source_width * state->height <=
		(uint64_t)source_height * state->width;
	margin = width_dominant ?
		(uint64_t)source_height * state->width - (uint64_t)state->height * source_width :
		(uint64_t)source_width * state->height - (uint64_t)state->width * source_height;
	for (uint32_t column = 0; column < state->width; column++)
		state->wallpaper_map[column] = width_dominant ?
			(uint32_t)((uint64_t)column * source_width / state->width) :
			(uint32_t)((2 * (uint64_t)column * source_height + margin) /
				(2 * (uint64_t)state->height));
	for (uint32_t row = 0; row < state->height; row++) {
		uint32_t source_row = width_dominant ?
			(uint32_t)((2 * (uint64_t)row * source_width + margin) /
				(2 * (uint64_t)state->width)) :
			(uint32_t)((uint64_t)row * source_height / state->height);

		state->wallpaper_map[state->width + row] = source_row * (uint32_t)state->wallpaper->stride;
	}
	state->wallpaper_width = state->width;
	state->wallpaper_height = state->height;
}

static uint32_t desktop_menu_top(const desktop_state *state)
{
	uint32_t panel_y = desktop_panel_y(state);
	uint32_t menu_height = desktop_button_height(state) * DESKTOP_MENU_ITEM_COUNT;

	return panel_y > menu_height ? panel_y - menu_height : 0;
}

static const char *desktop_menu_label(desktop_menu_entry entry)
{
	switch (entry) {
	case DESKTOP_MENU_TERMINAL: return "Terminal";
	default: return "";
	}
}

static int desktop_menu_entry_at(const desktop_state *state, uint32_t x,
				 uint32_t y)
{
	uint32_t menu_top;

	if (!state->menu_open || x < DESKTOP_MENU_X ||
	    x >= DESKTOP_MENU_X + desktop_menu_width(state))
		return -1;
	menu_top = desktop_menu_top(state);
	if (y < menu_top || y >= desktop_panel_y(state))
		return -1;
	return (int)((y - menu_top) / desktop_button_height(state));
}

static uint32_t desktop_focused_window_id(const desktop_state *state)
{
	for (const desktop_window *window = state->windows; window != NULL; window = window->next)
		if (window->focused)
			return window->window_id;
	return 0;
}

static void desktop_draw_label(desktop_state *state, const char *text, gui_rect rectangle)
{
	gui_surface clipped;
	int mask_top = (int)desktop_menu_top(state);
	int height = state->font_metrics.ascent - state->font_metrics.descent + state->font_metrics.line_gap;
	int baseline = rectangle.h > height ? (rectangle.h - height) / 2 : 0;

	rectangle.y -= mask_top;
	if (rectangle.x < 0 || rectangle.y < 0 || rectangle.x >= state->text_surface->width ||
	    rectangle.y >= state->text_surface->height || rectangle.w <= 0 || rectangle.h <= 0)
		return;
	if (rectangle.w > state->text_surface->width - rectangle.x)
		rectangle.w = state->text_surface->width - rectangle.x;
	if (rectangle.h > state->text_surface->height - rectangle.y)
		rectangle.h = state->text_surface->height - rectangle.y;
	clipped = (gui_surface) { .width = rectangle.w, .height = rectangle.h,
		.stride = state->text_surface->stride, .pixels = state->text_surface->pixels +
		(size_t)rectangle.y * state->text_surface->stride + rectangle.x };
	gui_font_draw_text(&clipped, 4, baseline + state->font_metrics.ascent,
		state->font, text, 0x00ffffff);
}

static void desktop_prepare_text(desktop_state *state)
{
	uint32_t top = desktop_menu_top(state);
	uint32_t panel_y = desktop_panel_y(state);

	if (state->font == NULL || state->width == 0 || state->height <= top)
		return;
	if (state->text_surface == NULL || (uint32_t)state->text_surface->width != state->width ||
	    (uint32_t)state->text_surface->height != state->height - top) {
		gui_surface *surface = gui_surface_create(state->width, state->height - top);

		gui_surface_destroy(state->text_surface);
		state->text_surface = surface;
		if (surface == NULL) {
			fprintf(stderr, "mario-desktop: text cache allocation failed; using bitmap text\n");
			return;
		}
	}
	gui_fill_rect(state->text_surface, (gui_rect) { 0, 0, state->text_surface->width,
		state->text_surface->height }, 0);
	desktop_draw_label(state, "MENU", (gui_rect) { 8, panel_y + 4,
		desktop_launcher_width(state), desktop_button_height(state) });
	if (state->width >= desktop_clock_width(state))
		desktop_draw_label(state, "--:--", (gui_rect) { state->width - desktop_clock_width(state),
			panel_y + 4, desktop_clock_width(state), desktop_button_height(state) });
	for (size_t index = 0; index < state->painted_taskbar.count; index++)
		desktop_draw_label(state, state->painted_taskbar.buttons[index].label, (gui_rect) {
			state->painted_taskbar.first_x + index * desktop_taskbar_stride(state), panel_y + 4,
			desktop_taskbar_width(state), desktop_button_height(state) });
	if (state->menu_open)
		for (unsigned int entry = 0; entry < DESKTOP_MENU_ITEM_COUNT; entry++)
			desktop_draw_label(state, desktop_menu_label((desktop_menu_entry)entry), (gui_rect) {
				DESKTOP_MENU_X + 8, top + entry * desktop_button_height(state),
				desktop_menu_width(state) - 8, desktop_button_height(state) });
}

static uint32_t desktop_background_pixel(const desktop_state *state, uint32_t offset)
{
	uint32_t x = offset % state->width;
	uint32_t y = offset / state->width;
	uint32_t panel_y = desktop_panel_y(state);
	const desktop_button *button;
	int control;

	if (state->menu_open && x >= DESKTOP_MENU_X &&
	    x < DESKTOP_MENU_X + desktop_menu_width(state) && y >= desktop_menu_top(state) &&
	    y < panel_y) {
		int entry = desktop_menu_entry_at(state, x, y);

		if (entry >= 0 && (size_t)entry < DESKTOP_MENU_ITEM_COUNT) {
			uint32_t row_y = desktop_menu_top(state) +
				(uint32_t)entry * desktop_button_height(state);

			if (desktop_bitmap_text(state, desktop_menu_label((desktop_menu_entry)entry), x, y,
				DESKTOP_MENU_X + 12, row_y + 5))
				return gui_appearance_color(&state->appearance, GUI_APPEARANCE_FOREGROUND);
			return gui_appearance_color(&state->appearance, GUI_APPEARANCE_ACTIVE);
		}
		return gui_appearance_color(&state->appearance, GUI_APPEARANCE_PANEL);
	}
	if (y < panel_y) {
		if (state->wallpaper != NULL && state->wallpaper_map != NULL &&
		    state->wallpaper_width == state->width && state->wallpaper_height == state->height)
			return state->wallpaper->pixels[state->wallpaper_map[state->width + y] +
				state->wallpaper_map[x]];
		return gui_appearance_color(&state->appearance, GUI_APPEARANCE_BACKGROUND);
	}
	if (x >= 8 && x < 8 + desktop_launcher_width(state) &&
	    y >= panel_y + 4 && y < panel_y + 4 + desktop_button_height(state)) {
		if (desktop_bitmap_text(state, "MENU", x, y, 12, panel_y + 9))
			return gui_appearance_color(&state->appearance, GUI_APPEARANCE_FOREGROUND);
		return gui_appearance_color(&state->appearance, GUI_APPEARANCE_ACTIVE);
	}
	control = desktop_taskbar_control(state, &state->painted_taskbar, x, y);
	if (control != 0) {
		uint32_t origin = control < 0 ? desktop_taskbar_first_x(state) :
			state->width - desktop_clock_width(state) - DESKTOP_TASKBAR_CONTROL_STRIDE;
		uint32_t row = y - panel_y - 4;
		int enabled = control < 0 ? state->painted_taskbar.page != 0 :
			state->painted_taskbar.page + 1 < state->painted_taskbar.pages;

		if (row >= 5 && row < 15) {
			uint32_t distance = row < 10 ? 9 - row : row - 10;
			uint32_t column = control < 0 ? 6 + distance : 13 - distance;

			if (x - origin >= column && x - origin < column + 2)
				return gui_appearance_color(&state->appearance,
					enabled ? GUI_APPEARANCE_FOREGROUND : GUI_APPEARANCE_INACTIVE);
		}
		return gui_appearance_color(&state->appearance,
			enabled ? GUI_APPEARANCE_INACTIVE : GUI_APPEARANCE_PANEL);
	}
	button = desktop_taskbar_button(state, &state->painted_taskbar, x, y);
	if (button != NULL) {
		if (state->painted_taskbar.paged && desktop_bitmap_text(state, button->label, x, y,
			state->painted_taskbar.first_x +
			(button - state->painted_taskbar.buttons) * desktop_taskbar_stride(state) + 8,
			panel_y + 9))
			return gui_appearance_color(&state->appearance, GUI_APPEARANCE_FOREGROUND);
		return gui_appearance_color(&state->appearance,
			button->focused ? GUI_APPEARANCE_ACTIVE : GUI_APPEARANCE_INACTIVE);
	}
	if (state->width >= desktop_clock_width(state) &&
	    desktop_bitmap_text(state, "--:--", x, y, state->width - desktop_clock_width(state), panel_y + 9))
		return gui_appearance_color(&state->appearance, GUI_APPEARANCE_FOREGROUND);
	if (state->launch_failed && x >= 8 + desktop_launcher_width(state) &&
	    x < desktop_taskbar_first_x(state) &&
	    y >= panel_y + 9 && y < panel_y + 19)
		return gui_appearance_color(&state->appearance, GUI_APPEARANCE_ERROR);
	return gui_appearance_color(&state->appearance, GUI_APPEARANCE_PANEL);
}

static uint32_t desktop_pixel(const desktop_state *state, uint32_t offset)
{
	uint32_t background = desktop_background_pixel(state, offset);
	uint32_t top;
	uint32_t row;
	uint32_t alpha;
	uint32_t foreground;
	uint32_t result = 0;

	if (state->font == NULL || state->text_surface == NULL)
		return background;
	top = desktop_menu_top(state);
	row = offset / state->width;
	if (row < top)
		return background;
	alpha = state->text_surface->pixels[(size_t)(row - top) * state->text_surface->stride +
		offset % state->width] & 255;
	if (alpha == 0)
		return background;
	foreground = gui_appearance_color(&state->appearance, GUI_APPEARANCE_FOREGROUND);
	for (unsigned int shift = 0; shift < 24; shift += 8)
		result |= ((((foreground >> shift) & 255) * alpha +
			((background >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
	return result;
}

static int read_message(int fd, gui_protocol_header *header, uint8_t *payload,
				size_t payload_capacity)
{
	uint8_t wire_header[GUI_PROTOCOL_HEADER_SIZE];

	read_full(fd, wire_header, sizeof(wire_header));
	if (gui_protocol_decode_header(wire_header, sizeof(wire_header), header) < 0 ||
	    header->length < GUI_PROTOCOL_HEADER_SIZE ||
	    header->length - GUI_PROTOCOL_HEADER_SIZE > payload_capacity)
		return -1;
	read_full(fd, payload, header->length - GUI_PROTOCOL_HEADER_SIZE);
	return 0;
}

static int desktop_read_message(gui_protocol_header *header, uint8_t *payload,
					size_t payload_capacity)
{
	return read_message(WM_FD, header, payload, payload_capacity);
}

static int desktop_activate_window(uint32_t window_id)
{
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_id)];

	if (gui_protocol_encode_header(frame, sizeof(frame),
		GUI_PROTOCOL_SHELL_ACTIVATE_WINDOW,
		sizeof(gui_protocol_wire_window_id)) < 0)
		return -1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, window_id);
	return write_full(WM_FD, frame, sizeof(frame));
}

static int send_message(int fd, gui_protocol_type type, const void *payload,
			 size_t payload_bytes)
{
	uint8_t header[GUI_PROTOCOL_HEADER_SIZE];

	if (gui_protocol_encode_header(header, sizeof(header), type, payload_bytes) < 0 ||
	    write_full(fd, header, sizeof(header)) < 0 ||
	    (payload_bytes != 0 && write_full(fd, payload, payload_bytes) < 0))
		return -1;
	return 0;
}

static int desktop_send_work_area(const desktop_state *state)
{
	uint8_t payload[sizeof(gui_protocol_wire_shell_work_area)];
	uint32_t work_height = desktop_panel_y(state);

	put32(payload, 0);
	put32(payload + 4, 0);
	put32(payload + 8, state->width);
	put32(payload + 12, work_height);
	return send_message(WM_FD, GUI_PROTOCOL_SHELL_WORK_AREA, payload, sizeof(payload));
}

static int desktop_restore_menu_focus(desktop_state *state)
{
	uint32_t window_id = state->menu_restore_window_id;

	state->menu_restore_window_id = 0;
	if (window_id != 0 && desktop_window_by_id(state, window_id) != NULL &&
	    desktop_activate_window(window_id) < 0)
		return -1;
	return 0;
}

static int desktop_open_menu(desktop_state *state)
{
	if (state->menu_open)
		return 0;
	state->menu_restore_window_id = desktop_focused_window_id(state);
	if (state->menu_restore_window_id == 0)
		state->menu_restore_window_id = state->last_focused_window_id;
	state->menu_open = 1;
	state->redraw = 1;
	return 0;
}

static int desktop_close_menu(desktop_state *state)
{
	if (!state->menu_open)
		return 0;
	state->menu_open = 0;
	state->redraw = 1;
	return desktop_restore_menu_focus(state);
}

static int desktop_send_launch(desktop_state *state,
			       gui_protocol_launch_type launch_type,
			       const char *path)
{
	uint8_t payload[sizeof(gui_protocol_wire_launch_request) +
		GUI_PROTOCOL_MAX_PATH_BYTES];
	size_t path_bytes = path == NULL ? 0 : strlen(path);
	uint32_t request_id;

	if (path_bytes > GUI_PROTOCOL_MAX_PATH_BYTES ||
	    state->pending_request_id != 0)
		return -1;
	request_id = ++state->next_request_id;
	if (request_id == 0)
		request_id = ++state->next_request_id;
	put32(payload, request_id);
	put16(payload + 4, launch_type);
	put16(payload + 6, path_bytes);
	if (path_bytes != 0)
		memcpy(payload + sizeof(gui_protocol_wire_launch_request), path, path_bytes);
	if (send_message(WM_FD, GUI_PROTOCOL_LAUNCH_REQUEST, payload,
		sizeof(gui_protocol_wire_launch_request) + path_bytes) < 0)
		return -1;
	state->pending_request_id = request_id;
	state->launch_failed = 0;
	state->redraw = 1;
	return 0;
}

static int desktop_handle_window_list(desktop_state *state,
				      gui_protocol_type type, const uint8_t *payload,
				      size_t payload_bytes)
{
	desktop_window *window;
	desktop_window **link;
	uint32_t window_id;
	uint16_t title_bytes;

	if (payload_bytes < sizeof(gui_protocol_wire_window_list))
		return -1;
	window_id = get32(payload);
	title_bytes = (uint16_t)payload[28] | (uint16_t)payload[29] << 8;
	if (window_id == 0 || title_bytes > GUI_PROTOCOL_MAX_TITLE_BYTES ||
	    payload_bytes != sizeof(gui_protocol_wire_window_list) + title_bytes)
		return -1;
	link = &state->windows;
	while (*link != NULL && (*link)->window_id < window_id)
		link = &(*link)->next;
	if (type == GUI_PROTOCOL_WINDOW_LIST_REMOVE) {
		window = desktop_window_by_id(state, window_id);
		if (window == NULL)
			return -1;
		*link = window->next;
		state->record_bytes -= sizeof(*window);
		state->window_count--;
		free(window);
		state->redraw = 1;
		return 0;
	}
	window = desktop_window_by_id(state, window_id);
	if (type == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT ||
	    type == GUI_PROTOCOL_WINDOW_LIST_ADD) {
		if (window == NULL) {
			if (state->record_bytes > APP_GUI_DESKTOP_RECORD_BYTES - sizeof(*window))
				return -1;
			window = calloc(1, sizeof(*window));
			if (window == NULL)
				return -1;
			window->next = *link;
			*link = window;
			state->record_bytes += sizeof(*window);
			state->window_count++;
			state->redraw = 1;
		}
		if (window == NULL)
			return -1;
	} else if (window == NULL) {
		return -1;
	}
	int newly_focused = get32(payload + 20) && !window->focused;
	int focus_changed = get32(payload + 20) != window->focused;

	window->window_id = window_id;
	window->focused = get32(payload + 20);
	window->order = get32(payload + 24);
	if (window->focused)
		state->last_focused_window_id = window_id;
	if (newly_focused)
		desktop_reveal_window(state, window);
	state->redraw |= focus_changed;
	return 0;
}

static int desktop_handle_message(desktop_state *state,
				  const gui_protocol_header *header,
				  const uint8_t *payload, size_t payload_bytes)
{
	int flow_result = app_gui_surface_flow_message(&state->surface_flow,
		state->window_id, header->type, payload, payload_bytes);

	if (flow_result != 0)
		return flow_result < 0 ? -1 : 0;
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != state->window_id ||
		    get64(payload + 4) <= state->configure_serial ||
		    get32(payload + 12) != state->width || get32(payload + 16) != state->height)
			return -1;
		state->configure_serial = get64(payload + 4);
		app_gui_surface_defer(&state->surface_flow);
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_LIST_SNAPSHOT ||
	    header->type == GUI_PROTOCOL_WINDOW_LIST_ADD ||
	    header->type == GUI_PROTOCOL_WINDOW_LIST_CHANGE ||
	    header->type == GUI_PROTOCOL_WINDOW_LIST_REMOVE)
		return desktop_handle_window_list(state, header->type, payload, payload_bytes);
	if (header->type == GUI_PROTOCOL_LAUNCH_RESULT) {
		uint32_t request_id;
		int32_t status;

		if (payload_bytes != sizeof(gui_protocol_wire_launch_result))
			return -1;
		request_id = get32(payload);
		status = (int32_t)get32(payload + 4);
		if (request_id != state->pending_request_id)
			return -1;
		state->pending_request_id = 0;
		if (status != 0) {
			state->launch_failed = 1;
			fprintf(stderr, "mario-desktop: launch %u failed: %d\n",
				request_id, status);
		}
		state->redraw = 1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_INPUT_RESET) {
		if (payload_bytes != 0)
			return -1;
		state->mouse_buttons = 0;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		uint8_t buttons;
		uint32_t x;
		uint32_t y;
		uint32_t panel_y = desktop_panel_y(state);
		const desktop_window *window;
		int control;
		int menu_entry;
		int menu_button;

		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != state->window_id)
			return -1;
		if (get32(payload + 4) == GUI_INPUT_EVENT_KEY) {
			if (state->menu_open && get32(payload + 8) == GUI_KEYSYM_ESCAPE &&
			    desktop_close_menu(state) < 0)
				return -1;
			return 0;
		}
		if (get32(payload + 4) != GUI_INPUT_EVENT_MOUSE)
			return -1;
		x = get32(payload + 16);
		y = get32(payload + 20);
		buttons = payload[24];
		menu_entry = desktop_menu_entry_at(state, x, y);
		menu_button = x >= 8 && x < 8 + desktop_launcher_width(state) &&
			y >= panel_y + 4 && y < panel_y + 4 + desktop_button_height(state);
		if (!(state->mouse_buttons & 1) && (buttons & 1)) {
			if (menu_entry >= 0) {
				int result;

				if (state->pending_request_id != 0)
					return -1;
				switch ((desktop_menu_entry)menu_entry) {
				case DESKTOP_MENU_TERMINAL:
					result = desktop_send_launch(state,
						GUI_PROTOCOL_LAUNCH_TERMINAL, NULL);
					break;
				default:
					result = -1;
					break;
				}
				if (result < 0)
					return -1;
				state->menu_restore_window_id = 0;
				state->menu_open = 0;
				state->mouse_buttons = buttons;
				return 0;
			}
			if (menu_button) {
				if (state->menu_open) {
					if (desktop_close_menu(state) < 0)
						return -1;
				} else if (desktop_open_menu(state) < 0) {
					return -1;
				}
				state->mouse_buttons = buttons;
				return 0;
			}
			if (state->menu_open && desktop_close_menu(state) < 0)
				return -1;
		}
		control = desktop_taskbar_control(state, &state->displayed_taskbar, x, y);
		if (!(state->mouse_buttons & 1) && (buttons & 1) && control != 0) {
			if (control < 0 && state->displayed_taskbar.page != 0) {
				state->taskbar_page = state->displayed_taskbar.page - 1;
				state->redraw = 1;
			} else if (control > 0 && state->displayed_taskbar.page + 1 <
				   state->displayed_taskbar.pages) {
				state->taskbar_page = state->displayed_taskbar.page + 1;
				state->redraw = 1;
			}
			state->mouse_buttons = buttons;
			return 0;
		}
		window = desktop_taskbar_window(state, x, y);
		if (!(state->mouse_buttons & 1) && (buttons & 1) && window != NULL &&
		    desktop_activate_window(window->window_id) < 0)
			return -1;
		state->mouse_buttons = buttons;
		return 0;
	}
	return -1;
}

static int read_surface_data_reply(desktop_state *state, uint64_t frame_serial,
				   uint32_t pixel_offset, uint32_t pixel_count)
{
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];

	for (;;) {
		gui_protocol_header header;
		uint64_t configure_serial = state->configure_serial;

		if (desktop_read_message(&header, payload, sizeof(payload)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY &&
		    !state->surface_flow.discard_reply) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(payload) != state->window_id ||
			    get64(payload + 4) != state->configure_serial ||
			    get64(payload + 12) != frame_serial ||
			    get32(payload + 20) != pixel_offset ||
			    get32(payload + 24) != pixel_count)
				return -1;
			return 0;
		}
		if (desktop_handle_message(state, &header, payload,
			    header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_WINDOW_CONFIGURE ||
		    (header.type == GUI_PROTOCOL_ERROR &&
		     ((uint16_t)payload[4] | (uint16_t)payload[5] << 8) ==
		     GUI_PROTOCOL_SURFACE_BEGIN && state->surface_flow.retry_pending)) {
			app_gui_surface_discard_reply(&state->surface_flow, configure_serial,
				frame_serial, pixel_offset, pixel_count);
			return 1;
		}
	}
}

static int send_desktop(desktop_state *state)
{
	desktop_state scene;
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++state->frame_serial;
	uint32_t total;

	if (desktop_prepare_taskbar(state) < 0 || frame_serial == 0 || (state->width != 0 &&
	    state->height > UINT32_MAX / state->width))
		return -1;
	total = state->width * state->height;
	desktop_prepare_wallpaper(state);
	desktop_prepare_text(state);
	scene = *state;

	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
		sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + 8, state->window_id);
	put64(begin + 12, state->configure_serial);
	put64(begin + 20, frame_serial);
	put32(begin + 36, state->width); put32(begin + 40, state->height);
	if (write_full(WM_FD, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixels = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload = sizeof(gui_protocol_wire_surface_data) +
			pixels * sizeof(uint32_t);
		int result;

		/* The protocol keeps each frame below Mario's one-page socket ring. */
		if (gui_protocol_encode_header(desktop_frame, sizeof(desktop_frame),
			GUI_PROTOCOL_SURFACE_DATA, payload) < 0)
			return -1;
		put32(desktop_frame + 8, state->window_id);
		put64(desktop_frame + 12, state->configure_serial);
		put64(desktop_frame + 20, frame_serial);
		put32(desktop_frame + 28, pixel_offset); put32(desktop_frame + 32, pixels);
		/* Build the complete shell scene incrementally to stay within the socket ring. */
		for (uint32_t index = 0; index < pixels; index++)
			put32(desktop_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				desktop_pixel(&scene, pixel_offset + index));
		if (write_full(WM_FD, desktop_frame,
			GUI_PROTOCOL_HEADER_SIZE + payload) < 0)
			return -1;
		result = read_surface_data_reply(state, frame_serial, pixel_offset, pixels);
		if (result != 0)
			return result;
		pixel_offset += pixels;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
		sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + 8, state->window_id);
	put64(commit + 12, state->configure_serial);
	put64(commit + 20, frame_serial);
	if (write_full(WM_FD, commit, sizeof(commit)) < 0)
		return -1;
	desktop_commit_taskbar(state);
	return 0;
}

static void diagnostic_reset(diagnostic_client *client)
{
	if (client->fd >= 0)
		close(client->fd);
	*client = (diagnostic_client) { .fd = -1 };
}

static int diagnostic_handle_message(diagnostic_client *client,
				     const gui_protocol_header *header,
				     const uint8_t *payload, size_t payload_bytes)
{
	int flow_result = app_gui_surface_flow_message(&client->surface_flow,
		client->window_id, header->type, payload, payload_bytes);

	if (flow_result != 0)
		return flow_result < 0 ? -1 : 0;
	if (header->type == GUI_PROTOCOL_WINDOW_CONFIGURE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_configure) ||
		    get32(payload) != client->window_id ||
		    get64(payload + 4) <= client->configure_serial ||
		    get32(payload + 12) == 0 || get32(payload + 16) == 0)
			return -1;
		client->configure_serial = get64(payload + 4);
		app_gui_surface_defer(&client->surface_flow);
		client->width = get32(payload + 12);
		client->height = get32(payload + 16);
		client->redraw = 1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_CLOSE_REQUEST) {
		uint8_t destroy[sizeof(gui_protocol_wire_window_id)];

		if (payload_bytes != sizeof(destroy) || get32(payload) != client->window_id)
			return -1;
		put32(destroy, client->window_id);
		if (send_message(client->fd, GUI_PROTOCOL_WINDOW_DESTROY, destroy,
				 sizeof(destroy)) < 0)
			return -1;
		/* Closing the ordinary endpoint makes launcher recreation unambiguous. */
		diagnostic_reset(client);
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_STATE) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_state) ||
		    get32(payload) != client->window_id ||
		    get32(payload + 4) > GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED)
			return -1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_WINDOW_FOCUS) {
		if (payload_bytes != sizeof(gui_protocol_wire_window_focus) ||
		    get32(payload) != client->window_id || get32(payload + 4) > 1)
			return -1;
		return 0;
	}
	if (header->type == GUI_PROTOCOL_INPUT_RESET)
		return payload_bytes == 0 ? 0 : -1;
	if (header->type == GUI_PROTOCOL_INPUT_EVENT) {
		if (payload_bytes != sizeof(gui_protocol_wire_input_event) ||
		    get32(payload) != client->window_id)
			return -1;
		return 0;
	}
	return -1;
}

static int diagnostic_read_surface_reply(diagnostic_client *client,
					 uint64_t frame_serial, uint32_t pixel_offset,
					 uint32_t pixel_count)
{
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];

	for (;;) {
		gui_protocol_header header;
		uint64_t configure_serial = client->configure_serial;

		if (read_message(client->fd, &header, payload, sizeof(payload)) < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_SURFACE_DATA_REPLY &&
		    !client->surface_flow.discard_reply) {
			if (header.length != GUI_PROTOCOL_HEADER_SIZE +
			    sizeof(gui_protocol_wire_surface_data_reply) ||
			    get32(payload) != client->window_id ||
			    get64(payload + 4) != client->configure_serial ||
			    get64(payload + 12) != frame_serial ||
			    get32(payload + 20) != pixel_offset ||
			    get32(payload + 24) != pixel_count)
				return -1;
			return 0;
		}
		if (diagnostic_handle_message(client, &header, payload,
			    header.length - GUI_PROTOCOL_HEADER_SIZE) < 0 || client->fd < 0)
			return -1;
		if (header.type == GUI_PROTOCOL_WINDOW_CONFIGURE ||
		    (header.type == GUI_PROTOCOL_ERROR &&
		     ((uint16_t)payload[4] | (uint16_t)payload[5] << 8) ==
		     GUI_PROTOCOL_SURFACE_BEGIN && client->surface_flow.retry_pending)) {
			app_gui_surface_discard_reply(&client->surface_flow, configure_serial,
				frame_serial, pixel_offset, pixel_count);
			return 1;
		}
	}
}

static int send_diagnostic(diagnostic_client *client)
{
	uint8_t begin[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_begin)] = { 0 };
	uint8_t commit[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_surface_commit)];
	uint64_t frame_serial = ++client->frame_serial;
	uint32_t total;

	if (frame_serial == 0 || (client->width != 0 &&
	    client->height > UINT32_MAX / client->width))
		return -1;
	total = client->width * client->height;
	if (gui_protocol_encode_header(begin, sizeof(begin), GUI_PROTOCOL_SURFACE_BEGIN,
		sizeof(gui_protocol_wire_surface_begin)) < 0)
		return -1;
	put32(begin + 8, client->window_id);
	put64(begin + 12, client->configure_serial);
	put64(begin + 20, frame_serial);
	put32(begin + 36, client->width); put32(begin + 40, client->height);
	if (write_full(client->fd, begin, sizeof(begin)) < 0)
		return -1;
	for (uint32_t pixel_offset = 0; pixel_offset < total;) {
		uint32_t pixels = total - pixel_offset > SURFACE_PIXELS_PER_MESSAGE ?
			SURFACE_PIXELS_PER_MESSAGE : total - pixel_offset;
		size_t payload = sizeof(gui_protocol_wire_surface_data) +
			pixels * sizeof(uint32_t);
		int result;

		if (gui_protocol_encode_header(desktop_frame, sizeof(desktop_frame),
			GUI_PROTOCOL_SURFACE_DATA, payload) < 0)
			return -1;
		put32(desktop_frame + 8, client->window_id);
		put64(desktop_frame + 12, client->configure_serial);
		put64(desktop_frame + 20, frame_serial);
		put32(desktop_frame + 28, pixel_offset); put32(desktop_frame + 32, pixels);
		for (uint32_t index = 0; index < pixels; index++)
			put32(desktop_frame + GUI_PROTOCOL_HEADER_SIZE +
				sizeof(gui_protocol_wire_surface_data) + index * sizeof(uint32_t),
				client->color);
		if (write_full(client->fd, desktop_frame, GUI_PROTOCOL_HEADER_SIZE + payload) < 0)
			return -1;
		result = diagnostic_read_surface_reply(client, frame_serial, pixel_offset, pixels);
		if (result != 0)
			return result;
		pixel_offset += pixels;
	}
	if (gui_protocol_encode_header(commit, sizeof(commit), GUI_PROTOCOL_SURFACE_COMMIT,
		sizeof(gui_protocol_wire_surface_commit)) < 0)
		return -1;
	put32(commit + 8, client->window_id);
	put64(commit + 12, client->configure_serial);
	put64(commit + 20, frame_serial);
	return write_full(client->fd, commit, sizeof(commit));
}

static int diagnostic_connect(diagnostic_client *client, unsigned int index)
{
	const diagnostic_definition *definition = &diagnostic_definitions[index];
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	uint8_t frame[GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create) +
		GUI_PROTOCOL_MAX_TITLE_BYTES] = { 0 };
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];
	gui_protocol_header header;
	size_t title_bytes = strlen(definition->title);
	size_t create_bytes = sizeof(gui_protocol_wire_window_create) + title_bytes;

	if (title_bytes > GUI_PROTOCOL_MAX_TITLE_BYTES ||
	    (client->fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;
	strcpy(address.sun_path, WM_SOCKET_PATH);
	if (connect(client->fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
	    gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(client->fd, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0 ||
	    read_message(client->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		goto fail;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_WINDOW_CREATE,
		create_bytes) < 0)
		goto fail;
	put32(frame + 8, index + 1);
	put32(frame + 12, definition->width);
	put32(frame + 16, definition->height);
	frame[20] = (uint8_t)title_bytes;
	frame[21] = (uint8_t)(title_bytes >> 8);
	memcpy(frame + GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_window_create),
		definition->title, title_bytes);
	if (write_full(client->fd, frame, GUI_PROTOCOL_HEADER_SIZE + create_bytes) < 0 ||
	    read_message(client->fd, &header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_WINDOW_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_window_create_reply) || get32(payload) != index + 1)
		goto fail;
	client->window_id = get32(payload + 4);
	client->configure_serial = get64(payload + 8);
	client->width = get32(payload + 16);
	client->height = get32(payload + 20);
	client->color = definition->color;
	client->x = definition->x;
	client->y = definition->y;
	if (client->window_id == 0 || client->configure_serial == 0 ||
	    client->width == 0 || client->height == 0)
		goto fail;
	/* Creation focus and reset are ordered before this client can draw. */
	for (unsigned int count = 0; count < 2; count++) {
		if (read_message(client->fd, &header, payload, sizeof(payload)) < 0 ||
		    diagnostic_handle_message(client, &header, payload,
			header.length - GUI_PROTOCOL_HEADER_SIZE) < 0)
			goto fail;
	}
	put32(frame, client->window_id);
	put32(frame + 4, (uint32_t)client->x);
	put32(frame + 8, (uint32_t)client->y);
	if (send_message(client->fd, GUI_PROTOCOL_WINDOW_MOVE, frame,
		sizeof(gui_protocol_wire_window_move)) < 0 || send_diagnostic(client) < 0)
		goto fail;
	return 0;

fail:
	diagnostic_reset(client);
	return -1;
}

static int private_fd_only(void)
{
	if (fcntl(WM_FD, F_GETFD) < 0)
		return -1;
	for (int fd = WM_FD + 1; fd < 1024; fd++) {
		if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
			return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	desktop_state state = { 0 };
	diagnostic_client diagnostics[DIAGNOSTIC_COUNT] = { 0 };
	int diagnostics_mode;
	uint8_t frame[64];
	uint8_t payload[sizeof(gui_protocol_wire_window_list) + GUI_PROTOCOL_MAX_TITLE_BYTES];
	gui_protocol_header header;
	struct pollfd pollfds[DIAGNOSTIC_COUNT + 1];

	for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++)
		diagnostics[index].fd = -1;

	if (argc == 2 && strcmp(argv[1], "--wm-fd=3") == 0) {
		diagnostics_mode = 0;
	} else if (argc == 3 && strcmp(argv[1], "--wm-fd=3") == 0 &&
		   strcmp(argv[2], "--diagnostics") == 0) {
		diagnostics_mode = 1;
	} else {
		fprintf(stderr, "mario-desktop: invalid inherited descriptors\n");
		return 1;
	}
	if (private_fd_only() < 0) {
		fprintf(stderr, "mario-desktop: invalid inherited descriptors\n");
		return 1;
	}
	if (gui_protocol_encode_hello(frame, sizeof(frame),
		&(gui_protocol_hello) { GUI_PROTOCOL_VERSION_MAJOR,
			GUI_PROTOCOL_VERSION_MINOR }) < 0 ||
	    write_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_hello)) < 0)
		return 1;
	if (desktop_read_message(&header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_HELLO_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE + sizeof(gui_protocol_wire_hello))
		return 1;
	if (gui_protocol_encode_header(frame, sizeof(frame), GUI_PROTOCOL_DESKTOP_CREATE,
		sizeof(gui_protocol_wire_desktop_create)) < 0)
		return 1;
	put32(frame + GUI_PROTOCOL_HEADER_SIZE, 1);
	if (write_full(WM_FD, frame, GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_desktop_create)) < 0)
		return 1;
	if (desktop_read_message(&header, payload, sizeof(payload)) < 0 ||
	    header.type != GUI_PROTOCOL_DESKTOP_CREATE_REPLY ||
	    header.length != GUI_PROTOCOL_HEADER_SIZE +
		sizeof(gui_protocol_wire_desktop_create_reply))
		return 1;
	state.window_id = get32(payload + 4);
	state.configure_serial = get64(payload + 8);
	state.width = get32(payload + 16);
	state.height = get32(payload + 20);
	if (state.window_id == 0 || state.configure_serial == 0)
		return 1;
	gui_appearance_load(&state.appearance);
	desktop_setup_font(&state);
	desktop_load_wallpaper(&state);
	if (desktop_send_work_area(&state) < 0)
		return 1;
	if (send_desktop(&state) < 0)
		return 1;
	if (diagnostics_mode)
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			if (diagnostic_connect(&diagnostics[index], index) < 0)
				return 1;
		}
	/* The WM may have forked after another shell wrote stdout. */
	if (write_full(STDOUT_FILENO, "mario-desktop: private connection ready\n",
		       sizeof("mario-desktop: private connection ready\n") - 1) < 0)
		return 1;
	for (;;) {
		if (state.redraw && !state.surface_flow.retry_pending) {
			state.redraw = 0;
			if (send_desktop(&state) < 0) {
				fprintf(stderr, "mario-desktop: desktop upload failed: %d\n", errno);
				return 1;
			}
			continue;
		}
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			if (diagnostics[index].fd >= 0 && diagnostics[index].redraw &&
			    !diagnostics[index].surface_flow.retry_pending) {
				diagnostics[index].redraw = 0;
				if (send_diagnostic(&diagnostics[index]) < 0)
					return 1;
				break;
			}
		}
		pollfds[0] = (struct pollfd) { .fd = WM_FD, .events = POLLIN };
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			pollfds[index + 1] = (struct pollfd) {
				.fd = diagnostics[index].fd,
				.events = diagnostics[index].fd >= 0 ? POLLIN : 0,
			};
		}
		int retry_pending = state.surface_flow.retry_pending;
		int poll_result;

		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++)
			retry_pending |= diagnostics[index].surface_flow.retry_pending;
		poll_result = poll(pollfds, DIAGNOSTIC_COUNT + 1,
			retry_pending ? APP_GUI_SURFACE_RETRY_MS : -1);
		if (poll_result < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		if (poll_result == 0) {
			if (app_gui_surface_retry_ready(&state.surface_flow)) {
				state.redraw = 1;
			}
			for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
				if (app_gui_surface_retry_ready(&diagnostics[index].surface_flow)) {
					diagnostics[index].redraw = 1;
				}
			}
			continue;
		}
		if (pollfds[0].revents & (POLLERR | POLLHUP | POLLNVAL) ||
		    (pollfds[0].revents & POLLIN &&
		     (desktop_read_message(&header, payload, sizeof(payload)) < 0 ||
		      desktop_handle_message(&state, &header, payload,
			header.length - GUI_PROTOCOL_HEADER_SIZE) < 0))) {
			fprintf(stderr, "mario-desktop: desktop event failed: %u, %u, %d\n",
				header.type, header.length, errno);
			return 1;
		}
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			if (!(pollfds[index + 1].revents & POLLIN))
				continue;
			if (read_message(diagnostics[index].fd, &header, payload,
			    sizeof(payload)) < 0 || diagnostic_handle_message(&diagnostics[index],
			    &header, payload, header.length - GUI_PROTOCOL_HEADER_SIZE) < 0) {
				fprintf(stderr, "mario-desktop: diagnostic event failed: %u, %u, %d\n",
					header.type, header.length, errno);
				return 1;
			}
		}
		if (app_gui_surface_retry_ready(&state.surface_flow))
			state.redraw = 1;
		for (unsigned int index = 0; index < DIAGNOSTIC_COUNT; index++) {
			if (app_gui_surface_retry_ready(&diagnostics[index].surface_flow))
				diagnostics[index].redraw = 1;
		}
	}
}
