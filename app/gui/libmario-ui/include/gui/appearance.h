#ifndef GUI_APPEARANCE_H
#define GUI_APPEARANCE_H

#include <stddef.h>
#include <stdint.h>

#define GUI_APPEARANCE_MAX_BYTES 65536U
#define GUI_APPEARANCE_PATH_MAX 256U
#define GUI_APPEARANCE_WALLPAPER_DEFAULT "/usr/share/mario/wallpaper.png"

typedef enum {
	GUI_APPEARANCE_BACKGROUND,
	GUI_APPEARANCE_PANEL,
	GUI_APPEARANCE_ACTIVE,
	GUI_APPEARANCE_INACTIVE,
	GUI_APPEARANCE_FOREGROUND,
	GUI_APPEARANCE_ERROR,
	GUI_APPEARANCE_BORDER,
	GUI_APPEARANCE_CONTENT,
	GUI_APPEARANCE_COLOR_COUNT,
} gui_appearance_color_id;

typedef struct {
	unsigned int font_size;
	char font_path[GUI_APPEARANCE_PATH_MAX];
	char wallpaper_path[GUI_APPEARANCE_PATH_MAX];
	uint32_t colors[GUI_APPEARANCE_COLOR_COUNT];
} gui_appearance;

extern const gui_appearance gui_appearance_default;

int gui_appearance_parse(gui_appearance *appearance, const char *contents, size_t bytes);
void gui_appearance_load(gui_appearance *appearance);
uint32_t gui_appearance_color(const gui_appearance *appearance, gui_appearance_color_id color);

#endif
