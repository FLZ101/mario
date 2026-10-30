#include <fs/tty/console.h>
#include <lib/string.h>
#include <lib/spinlock.h>
#include <sched.h>
#include <wait.h>
#include <video/fb.h>
#include <video/fbcon.h>

extern void kernel_thread(void (*fun)(void *), void *arg);

#define FONT_WIDTH 9
#define FONT_HEIGHT 16
#define LINE_GAP 2
#define CELL_HEIGHT (FONT_HEIGHT + LINE_GAP)
#define TERMINAL_PADDING_X FONT_WIDTH
#define TERMINAL_PADDING_Y CELL_HEIGHT
#define CONTENT_WIDTH (N_COL * FONT_WIDTH)
#define CONTENT_HEIGHT (N_ROW * CELL_HEIGHT)
#define CANVAS_WIDTH (CONTENT_WIDTH + 2 * TERMINAL_PADDING_X)
#define CANVAS_HEIGHT (CONTENT_HEIGHT + 2 * TERMINAL_PADDING_Y)

static const uint8_t font_cp437[256][16] = {
#include "font_cp437_8x16.inc"
};

static const uint8_t palette[16][3] = {
	{ 0, 0, 0 }, { 0, 0, 170 }, { 0, 170, 0 }, { 0, 170, 170 },
	{ 170, 0, 0 }, { 170, 0, 170 }, { 170, 85, 0 }, { 170, 170, 170 },
	{ 85, 85, 85 }, { 85, 85, 255 }, { 85, 255, 85 }, { 85, 255, 255 },
	{ 255, 85, 85 }, { 255, 85, 255 }, { 255, 255, 85 }, { 255, 255, 255 },
};

static wait_queue_t fbcon_wait;
static spinlock_t fbcon_lock = SPINLOCK_UNLOCKED;
static int fbcon_pending;
static int fbcon_started;
static int fbcon_full_repaint;

struct fbcon_layout {
	unsigned int left;
	unsigned int top;
	unsigned int width;
	unsigned int height;
};

struct fbcon_snapshot {
	uint16_t mem[N_ROW][N_COL];
	uint16_t last_mem[N_ROW][N_COL];
	unsigned int pos_y, pos_x;
	unsigned int last_pos_y, last_pos_x;
	int cursor_hidden;
	int last_cursor_hidden;
	int initialized;
};

static struct fbcon_snapshot snapshot;

int fbcon_enabled(void)
{
	return fb_is_enabled();
}

static uint32_t scale_component(uint8_t value, uint32_t bits)
{
	uint32_t maximum;
	if (!bits)
		return 0;
	maximum = (1U << bits) - 1;
	return (value * maximum + 127) / 255;
}

static void fbcon_pixel(unsigned int x, unsigned int y, const uint8_t rgb[3])
{
	const struct fb_info *fb = fb_get_info();
	uint32_t pixel;
	uint8_t *dst;

	if (x >= fb->width || y >= fb->height)
		return;
	pixel = (scale_component(rgb[0], fb->red.length) << fb->red.offset) |
		(scale_component(rgb[1], fb->green.length) << fb->green.offset) |
		(scale_component(rgb[2], fb->blue.length) << fb->blue.offset);
	dst = (uint8_t *)fb->virt_addr + y * fb->pitch + x * ((fb->bpp + 7) / 8);
	switch (fb->bpp) {
	case 15:
	case 16:
		*(uint16_t *)dst = pixel;
		break;
	case 24:
		dst[0] = pixel;
		dst[1] = pixel >> 8;
		dst[2] = pixel >> 16;
		break;
	default:
		*(uint32_t *)dst = pixel;
		break;
	}
}

static void fbcon_layout(struct fbcon_layout *layout)
{
	const struct fb_info *fb = fb_get_info();
	unsigned int canvas_left, canvas_top, canvas_width, canvas_height;
	unsigned int content_right, content_bottom;

	if ((uint64_t)fb->width * CANVAS_HEIGHT <=
	    (uint64_t)fb->height * CANVAS_WIDTH) {
		canvas_width = fb->width;
		canvas_height = (uint64_t)fb->width * CANVAS_HEIGHT / CANVAS_WIDTH;
	} else {
		canvas_height = fb->height;
		canvas_width = (uint64_t)fb->height * CANVAS_WIDTH / CANVAS_HEIGHT;
	}
	canvas_left = (fb->width - canvas_width) / 2;
	canvas_top = (fb->height - canvas_height) / 2;
	layout->left = canvas_left +
		(uint64_t)canvas_width * TERMINAL_PADDING_X / CANVAS_WIDTH;
	layout->top = canvas_top +
		(uint64_t)canvas_height * TERMINAL_PADDING_Y / CANVAS_HEIGHT;
	content_right = canvas_left + (uint64_t)canvas_width *
		(TERMINAL_PADDING_X + CONTENT_WIDTH) / CANVAS_WIDTH;
	content_bottom = canvas_top + (uint64_t)canvas_height *
		(TERMINAL_PADDING_Y + CONTENT_HEIGHT) / CANVAS_HEIGHT;
	layout->width = content_right - layout->left;
	layout->height = content_bottom - layout->top;
}

static unsigned int fbcon_glyph_bit(const uint8_t *glyph, unsigned int ch,
				   unsigned int x, unsigned int y)
{
	if (x < 8)
		return glyph[y] & (0x80 >> x) ? 255 : 0;
	return ch >= 0xc0 && ch <= 0xdf && (glyph[y] & 1) ? 255 : 0;
}

static unsigned int fbcon_sample_glyph(const uint8_t *glyph, unsigned int ch,
				      unsigned int x, unsigned int y,
				      unsigned int width, unsigned int height)
{
	int source_x = ((2 * x + 1) * FONT_WIDTH * 256) / (2 * width) - 128;
	int source_y = ((2 * y + 1) * FONT_HEIGHT * 256) / (2 * height) - 128;
	unsigned int x0, x1, y0, y1, x_fraction, y_fraction;
	unsigned int tl, tr, bl, br;

	if (source_x <= 0) {
		x0 = x1 = 0;
		x_fraction = 0;
	} else if (source_x >= (FONT_WIDTH - 1) * 256) {
		x0 = x1 = FONT_WIDTH - 1;
		x_fraction = 0;
	} else {
		x0 = source_x / 256;
		x1 = x0 + 1;
		x_fraction = source_x % 256;
	}
	if (source_y <= 0) {
		y0 = y1 = 0;
		y_fraction = 0;
	} else if (source_y >= (FONT_HEIGHT - 1) * 256) {
		y0 = y1 = FONT_HEIGHT - 1;
		y_fraction = 0;
	} else {
		y0 = source_y / 256;
		y1 = y0 + 1;
		y_fraction = source_y % 256;
	}
	tl = fbcon_glyph_bit(glyph, ch, x0, y0);
	tr = fbcon_glyph_bit(glyph, ch, x1, y0);
	bl = fbcon_glyph_bit(glyph, ch, x0, y1);
	br = fbcon_glyph_bit(glyph, ch, x1, y1);
	return (tl * (256 - x_fraction) * (256 - y_fraction) +
		tr * x_fraction * (256 - y_fraction) +
		bl * (256 - x_fraction) * y_fraction +
		br * x_fraction * y_fraction + 32768) >> 16;
}

static void fbcon_blend(uint8_t rgb[3], const uint8_t *fg, const uint8_t *bg,
			 unsigned int coverage)
{
	unsigned int channel;

	for (channel = 0; channel < 3; channel++)
		rgb[channel] = (fg[channel] * coverage +
				bg[channel] * (255 - coverage) + 127) / 255;
}

static void fbcon_glyph_bounds(unsigned int top, unsigned int bottom,
			       unsigned int *glyph_top, unsigned int *glyph_bottom)
{
	unsigned int cell_height = bottom - top;
	unsigned int glyph_height =
		(cell_height * FONT_HEIGHT + CELL_HEIGHT / 2) / CELL_HEIGHT;

	if (!glyph_height)
		glyph_height = 1;
	if (glyph_height > cell_height)
		glyph_height = cell_height;
	*glyph_top = top + (cell_height - glyph_height) / 2;
	*glyph_bottom = *glyph_top + glyph_height;
}

static void fbcon_cell(const struct fbcon_layout *layout, unsigned int row,
			unsigned int col, uint16_t cell)
{
	const uint8_t *fg = palette[(cell >> 8) & 0x0f];
	const uint8_t *bg = palette[(cell >> 12) & 0x0f];
	const uint8_t *glyph = font_cp437[cell & 0xff];
	unsigned int left = layout->left + col * layout->width / N_COL;
	unsigned int right = layout->left + (col + 1) * layout->width / N_COL;
	unsigned int top = layout->top + row * layout->height / N_ROW;
	unsigned int bottom = layout->top + (row + 1) * layout->height / N_ROW;
	unsigned int glyph_top, glyph_bottom;
	unsigned int x, y;

	fbcon_glyph_bounds(top, bottom, &glyph_top, &glyph_bottom);
	for (y = top; y < bottom; y++) {
		for (x = left; x < right; x++) {
			uint8_t rgb[3];
			unsigned int coverage = 0;

			if (y >= glyph_top && y < glyph_bottom)
				coverage = fbcon_sample_glyph(glyph, cell & 0xff,
					x - left, y - glyph_top, right - left,
					glyph_bottom - glyph_top);

			fbcon_blend(rgb, fg, bg, coverage);
			fbcon_pixel(x, y, rgb);
		}
	}
}

static void fbcon_cursor(const struct fbcon_layout *layout, unsigned int row,
			 unsigned int col, uint16_t cell)
{
	const uint8_t *color = palette[(cell >> 8) & 0x0f];
	unsigned int left = layout->left + col * layout->width / N_COL;
	unsigned int right = layout->left + (col + 1) * layout->width / N_COL;
	unsigned int top = layout->top + row * layout->height / N_ROW;
	unsigned int bottom = layout->top + (row + 1) * layout->height / N_ROW;
	unsigned int glyph_top, glyph_bottom;
	unsigned int underline_height;
	unsigned int underline_top;
	unsigned int x, y;

	fbcon_glyph_bounds(top, bottom, &glyph_top, &glyph_bottom);
	underline_height = ((glyph_bottom - glyph_top) * 2 + FONT_HEIGHT - 1) /
		FONT_HEIGHT;
	underline_top = glyph_bottom - underline_height;
	for (y = underline_top; y < glyph_bottom; y++)
		for (x = left; x < right; x++)
			fbcon_pixel(x, y, color);
}

static void fbcon_paint(int full)
{
	const struct fb_info *fb = fb_get_info();
	struct fbcon_layout layout;
	unsigned int x, y;

	if (fb->width < CANVAS_WIDTH || fb->height < N_ROW * FONT_HEIGHT)
		return;
	console_snapshot_foreground(snapshot.mem, &snapshot.pos_y, &snapshot.pos_x,
				    &snapshot.cursor_hidden);
	fbcon_layout(&layout);
	if (full || !snapshot.initialized)
		memset(fb->virt_addr, 0, fb->size);

	if (!full && !snapshot.last_cursor_hidden && snapshot.initialized &&
	    (snapshot.last_pos_x != snapshot.pos_x ||
	     snapshot.last_pos_y != snapshot.pos_y ||
	     snapshot.last_cursor_hidden != snapshot.cursor_hidden))
		fbcon_cell(&layout, snapshot.last_pos_y, snapshot.last_pos_x,
			   snapshot.mem[snapshot.last_pos_y][snapshot.last_pos_x]);

	for (y = 0; y < N_ROW; y++)
		for (x = 0; x < N_COL; x++)
			if (full || !snapshot.initialized ||
			    snapshot.mem[y][x] != snapshot.last_mem[y][x])
				fbcon_cell(&layout, y, x, snapshot.mem[y][x]);

	if (!snapshot.cursor_hidden)
		fbcon_cursor(&layout, snapshot.pos_y, snapshot.pos_x,
			     snapshot.mem[snapshot.pos_y][snapshot.pos_x]);

	memcpy(snapshot.last_mem, snapshot.mem, sizeof(snapshot.mem));
	snapshot.last_pos_x = snapshot.pos_x;
	snapshot.last_pos_y = snapshot.pos_y;
	snapshot.last_cursor_hidden = snapshot.cursor_hidden;
	snapshot.initialized = 1;
}

static void fbcon_thread(void *arg)
{
	(void)arg;
	for (;;) {
		int full;

		irq_save();
		ACQUIRE_LOCK(&fbcon_lock);
		if (!fbcon_pending) {
			sleep_on(&fbcon_wait, TASK_UNINTERRUPTIBLE, &fbcon_lock);
			continue;
		}
		full = fbcon_full_repaint;
		fbcon_pending = 0;
		fbcon_full_repaint = 0;
		RELEASE_LOCK(&fbcon_lock);
		irq_restore();
		fbcon_paint(full);
	}
}

static void fbcon_queue(int full)
{
	unsigned long flags;

	if (!fb_is_enabled())
		return;
	spin_lock_irqsave(&fbcon_lock, flags);
	fbcon_pending = 1;
	if (full)
		fbcon_full_repaint = 1;
	spin_unlock_irqrestore(&fbcon_lock, flags);
	wake_up_all(&fbcon_wait);
}

void fbcon_console_changed(struct console *con)
{
	(void)con;
	fbcon_queue(0);
}

void fbcon_console_switched(struct console *con)
{
	(void)con;
	fbcon_queue(1);
}

void fbcon_start(void)
{
	if (!fb_is_enabled() || fbcon_started)
		return;
	init_wait_queue(&fbcon_wait);
	fbcon_started = 1;
	kernel_thread(fbcon_thread, NULL);
	fbcon_queue(1);
}
