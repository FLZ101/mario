#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

const terminal_config terminal_config_default = {
	.font_size = 14, .opacity = 100,
	.foreground = 0x00f8fafc, .background = 0x00111827, .cursor = 0x00f8fafc,
	.palette = {
		0x00111827, 0x00ef4444, 0x0022c55e, 0x00eab308,
		0x003b82f6, 0x00d946ef, 0x0006b6d4, 0x00cbd5e1,
		0x00475569, 0x00f87171, 0x004ade80, 0x00facc15,
		0x0060a5fa, 0x00e879f9, 0x0022d3ee, 0x00f8fafc,
	},
};

static int config_number(const char *value, unsigned int minimum,
		unsigned int maximum, unsigned int *result)
{
	char *end;
	unsigned long number;

	if (!isdigit((unsigned char)*value))
		return -1;
	errno = 0;
	number = strtoul(value, &end, 10);
	if (errno || *end || number < minimum || number > maximum)
		return -1;
	*result = (unsigned int)number;
	return 0;
}

static int config_color(const char *value, uint32_t *result)
{
	char *end;
	unsigned long color;

	if (*value != '#' || strlen(value + 1) != 6)
		return -1;
	for (const char *digit = value + 1; *digit; digit++)
		if (!isxdigit((unsigned char)*digit))
			return -1;
	color = strtoul(value + 1, &end, 16);
	if (*end)
		return -1;
	*result = (uint32_t)color;
	return 0;
}

static char *config_trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text))
		text++;
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return text;
}

static int config_scheme(terminal_config *config, const char *name)
{
	if (!strcmp(name, "default") || !strcmp(name, "dark")) {
		config->foreground = terminal_config_default.foreground;
		config->background = terminal_config_default.background;
		config->cursor = terminal_config_default.cursor;
		memcpy(config->palette, terminal_config_default.palette, sizeof(config->palette));
	} else if (!strcmp(name, "light")) {
		config->foreground = 0x00111827;
		config->background = 0x00f8fafc;
		config->cursor = 0x00111827;
		memcpy(config->palette, terminal_config_default.palette, sizeof(config->palette));
		config->palette[7] = 0x00475569;
		config->palette[15] = 0x0064758b;
	} else if (!strcmp(name, "solarized-dark")) {
		static const uint32_t palette[] = {
			0x00073642, 0x00dc322f, 0x00859900, 0x00b58900,
			0x00268bd2, 0x00d33682, 0x002aa198, 0x00eee8d5,
			0x00002b36, 0x00cb4b16, 0x00586e75, 0x00657b83,
			0x00839496, 0x006c71c4, 0x0093a1a1, 0x00fdf6e3,
		};

		config->foreground = 0x00839496;
		config->background = 0x00002b36;
		config->cursor = 0x0093a1a1;
		memcpy(config->palette, palette, sizeof(config->palette));
	} else {
		return -1;
	}
	return 0;
}

static int config_setting(terminal_config *config, const char *key, const char *value)
{
	unsigned int index;

	if (!strcmp(key, "font_size"))
		return config_number(value, 8, 48, &config->font_size);
	if (!strcmp(key, "opacity"))
		return config_number(value, 0, 100, &config->opacity);
	if (!strcmp(key, "opaque")) {
		if (!strcmp(value, "true") || !strcmp(value, "1"))
			config->opacity = 100;
		else if (!strcmp(value, "false") || !strcmp(value, "0"))
			config->opacity = 80;
		else
			return -1;
		return 0;
	}
	if (!strcmp(key, "foreground"))
		return config_color(value, &config->foreground);
	if (!strcmp(key, "background"))
		return config_color(value, &config->background);
	if (!strcmp(key, "cursor"))
		return config_color(value, &config->cursor);
	if (!strncmp(key, "color", 5) && !config_number(key + 5, 0, 15, &index))
		return config_color(value, &config->palette[index]);
	return -1;
}

int terminal_config_parse(terminal_config *config, const char *contents, size_t bytes)
{
	if (config == NULL || contents == NULL || bytes > TERMINAL_CONFIG_MAX_BYTES) {
		errno = EINVAL;
		return -1;
	}
	*config = terminal_config_default;
	for (unsigned int pass = 0; pass < 2; pass++) {
		size_t offset = 0;
		unsigned int number = 0;

		while (offset < bytes) {
			char line[256];
			const char *newline = memchr(contents + offset, '\n', bytes - offset);
			size_t length = newline ? (size_t)(newline - contents - offset) : bytes - offset;
			char *key;
			char *value;
			int invalid = 0;

			number++;
			if (length >= sizeof(line) || memchr(contents + offset, '\0', length)) {
				invalid = 1;
			} else {
				memcpy(line, contents + offset, length);
				line[length] = '\0';
				key = config_trim(line);
				if (*key != '\0' && *key != '#') {
					value = strchr(key, '=');
					if (value == NULL) {
						invalid = 1;
					} else {
						*value++ = '\0';
						key = config_trim(key);
						value = config_trim(value);
						if (!strcmp(key, "color_scheme")) {
							if (pass == 0)
								invalid = config_scheme(config, value) < 0;
							else {
								terminal_config probe = *config;

								invalid = config_scheme(&probe, value) < 0;
							}
						} else if (pass == 1) {
							invalid = config_setting(config, key, value) < 0;
						}
					}
				}
			}
			if (invalid && pass == 1)
				fprintf(stderr, "terminal: invalid config setting on line %u; ignored\n", number);
			offset += length + (newline != NULL);
		}
	}
	return 0;
}

void terminal_config_load(terminal_config *config)
{
	const char *override = getenv("MARIO_TERMINAL_CONFIG");
	const char *base = getenv("XDG_CONFIG_HOME");
	const char *suffix = "/mario/terminal.conf";
	char *path = NULL;
	char *contents = NULL;
	struct stat status;
	size_t offset = 0;
	size_t bytes;
	int descriptor = -1;
	int error = 0;

	*config = terminal_config_default;
	if (override != NULL && *override == '\0')
		return;
	if (override == NULL) {
		if (base == NULL || *base == '\0') {
			base = getenv("HOME");
			suffix = "/.config/mario/terminal.conf";
		}
		if (base == NULL || strlen(base) + strlen(suffix) >= PATH_MAX)
			return;
		path = malloc(strlen(base) + strlen(suffix) + 1);
		if (path == NULL)
			return;
		strcpy(path, base);
		strcat(path, suffix);
	}
	descriptor = open(override ? override : path, O_RDONLY | O_NONBLOCK);
	if (descriptor < 0) {
		error = override || errno != ENOENT ? errno : 0;
		goto done;
	}
	if (fstat(descriptor, &status) < 0) {
		error = errno;
		goto done;
	}
	if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (uintmax_t)status.st_size > TERMINAL_CONFIG_MAX_BYTES) {
		error = EINVAL;
		goto done;
	}
	bytes = (size_t)status.st_size;
	contents = malloc(bytes + 1);
	if (contents == NULL) {
		error = ENOMEM;
		goto done;
	}
	while (offset < bytes) {
		ssize_t count = read(descriptor, contents + offset, bytes - offset);

		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0) {
			error = count == 0 ? EIO : errno;
			goto done;
		}
		offset += (size_t)count;
	}
	terminal_config_parse(config, contents, bytes);
done:
	if (error)
		fprintf(stderr, "terminal: config unavailable: %s: %s; using defaults\n",
			override ? override : path, strerror(error));
	if (descriptor >= 0)
		close(descriptor);
	free(contents);
	free(path);
}
