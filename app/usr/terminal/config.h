#ifndef TERMINAL_CONFIG_H
#define TERMINAL_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define TERMINAL_CONFIG_MAX_BYTES 65536U

typedef struct {
	unsigned int font_size;
	unsigned int opacity;
	uint32_t foreground;
	uint32_t background;
	uint32_t cursor;
	uint32_t palette[16];
} terminal_config;

extern const terminal_config terminal_config_default;

int terminal_config_parse(terminal_config *config, const char *contents, size_t bytes);
void terminal_config_load(terminal_config *config);

#endif
