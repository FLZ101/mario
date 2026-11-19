#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gui/appearance.h>

const gui_appearance gui_appearance_default = {
	.font_size = 10,
	.wallpaper_path = GUI_APPEARANCE_WALLPAPER_DEFAULT,
	.colors = { 0x001f2933, 0x00111827, 0x002563eb, 0x00475569,
		0x00f8fafc, 0x00ef4444, 0x000f172a, 0x00f8fafc },
};

uint32_t gui_appearance_color(const gui_appearance *appearance, gui_appearance_color_id color)
{
	return (appearance != NULL && appearance->font_size ? appearance : &gui_appearance_default)->colors[color];
}

static char *appearance_trim(char *text)
{
	char *end;

	while (isspace((unsigned char)*text))
		text++;
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return text;
}

static int appearance_scheme(gui_appearance *appearance, const char *name)
{
	static const uint32_t light[] = { 0x00f8fafc, 0x00e2e8f0, 0x003b82f6, 0x00cbd5e1,
		0x00111827, 0x00dc2626, 0x0064758b, 0x00f8fafc };
	static const uint32_t solarized[] = { 0x00002b36, 0x00073642, 0x00268bd2, 0x00586e75,
		0x00eee8d5, 0x00dc322f, 0x00657b83, 0x00073642 };
	const uint32_t *colors;

	if (!strcmp(name, "default") || !strcmp(name, "dark"))
		colors = gui_appearance_default.colors;
	else if (!strcmp(name, "light"))
		colors = light;
	else if (!strcmp(name, "solarized-dark"))
		colors = solarized;
	else
		return -1;
	if (appearance != NULL)
		memcpy(appearance->colors, colors, sizeof(appearance->colors));
	return 0;
}

static int appearance_font(gui_appearance *appearance, const char *name)
{
	static const struct { const char *family; const char *path; } fonts[] = {
		{ "bitmap", "" },
		{ "DejaVu Sans", "/usr/share/fonts/DejaVuSans.ttf" },
		{ "DejaVu Sans Bold", "/usr/share/fonts/DejaVuSans-Bold.ttf" },
		{ "DejaVu Sans Mono", "/usr/share/fonts/DejaVuSansMono.ttf" },
		{ "DejaVu Sans Mono Bold", "/usr/share/fonts/DejaVuSansMono-Bold.ttf" },
	};

	for (size_t index = 0; index < sizeof(fonts) / sizeof(fonts[0]); index++) {
		if (strcmp(name, fonts[index].family))
			continue;
		strcpy(appearance->font_path, fonts[index].path);
		return 0;
	}
	return -1;
}

static int appearance_setting(gui_appearance *appearance, const char *key, const char *value)
{
	static const char *keys[] = { "background", "panel", "active", "inactive",
		"foreground", "error", "border", "content" };
	char *end;
	unsigned long number;

	if (!strcmp(key, "font_family"))
		return appearance_font(appearance, value);
	if (!strcmp(key, "font_path")) {
		if (*value != '/' || strlen(value) >= sizeof(appearance->font_path))
			return -1;
		strcpy(appearance->font_path, value);
		return 0;
	}
	if (!strcmp(key, "wallpaper")) {
		if ((*value && *value != '/') || strlen(value) >= sizeof(appearance->wallpaper_path))
			return -1;
		strcpy(appearance->wallpaper_path, value);
		return 0;
	}
	if (!strcmp(key, "font_size")) {
		if (!isdigit((unsigned char)*value))
			return -1;
		errno = 0;
		number = strtoul(value, &end, 10);
		if (errno || *end || number < 8 || number > 32)
			return -1;
		appearance->font_size = (unsigned int)number;
		return 0;
	}
	if (*value != '#' || strlen(value) != 7)
		return -1;
	for (const char *digit = value + 1; *digit; digit++)
		if (!isxdigit((unsigned char)*digit))
			return -1;
	number = strtoul(value + 1, &end, 16);
	if (*end)
		return -1;
	for (size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
		if (strcmp(key, keys[index]))
			continue;
		appearance->colors[index] = (uint32_t)number;
		return 0;
	}
	return -1;
}

int gui_appearance_parse(gui_appearance *appearance, const char *contents, size_t bytes)
{
	int font_selected = 0;
	int size_selected = 0;

	if (appearance == NULL || contents == NULL || bytes > GUI_APPEARANCE_MAX_BYTES) {
		errno = EINVAL;
		return -1;
	}
	*appearance = gui_appearance_default;
	for (unsigned int pass = 0; pass < 2; pass++) {
		size_t offset = 0;
		unsigned int number = 0;

		while (offset < bytes) {
			char line[GUI_APPEARANCE_PATH_MAX + 32];
			const char *newline = memchr(contents + offset, '\n', bytes - offset);
			size_t length = newline ? (size_t)(newline - contents - offset) : bytes - offset;
			int invalid = 0;

			number++;
			if (length >= sizeof(line) || memchr(contents + offset, '\0', length)) {
				invalid = 1;
			} else {
				char *key;
				char *value;

				memcpy(line, contents + offset, length);
				line[length] = '\0';
				key = appearance_trim(line);
				if (*key && *key != '#') {
					value = strchr(key, '=');
					if (value == NULL) {
						invalid = 1;
					} else {
						*value++ = '\0';
						key = appearance_trim(key);
						value = appearance_trim(value);
						if (!strcmp(key, "color_scheme")) {
							invalid = appearance_scheme(pass == 0 ? appearance : NULL, value) < 0;
						} else if (pass == 1) {
							invalid = appearance_setting(appearance, key, value) < 0;
							if (!invalid && (!strcmp(key, "font_family") || !strcmp(key, "font_path")))
								font_selected = 1;
							if (!invalid && !strcmp(key, "font_size"))
								size_selected = 1;
						}
					}
				}
			}
			if (invalid && pass == 1)
				fprintf(stderr, "mario-gui: invalid desktop config setting on line %u; ignored\n", number);
			offset += length + (newline != NULL);
		}
	}
	if (size_selected && !font_selected)
		appearance_font(appearance, "DejaVu Sans");
	return 0;
}

void gui_appearance_load(gui_appearance *appearance)
{
	const char *override = getenv("MARIO_DESKTOP_CONFIG");
	const char *base = getenv("XDG_CONFIG_HOME");
	const char *suffix = "/mario/desktop.conf";
	char *path = NULL;
	char *contents = NULL;
	struct stat status;
	size_t offset = 0;
	size_t bytes;
	int descriptor = -1;
	int error = 0;

	*appearance = gui_appearance_default;
	if (override != NULL && *override == '\0')
		return;
	if (override == NULL) {
		if (base == NULL || *base == '\0') {
			base = getenv("HOME");
			suffix = "/.config/mario/desktop.conf";
		}
		if (base == NULL || *base == '\0' || strlen(base) + strlen(suffix) >= PATH_MAX)
			return;
		path = malloc(strlen(base) + strlen(suffix) + 1);
		if (path == NULL)
			return;
		strcpy(path, base);
		strcat(path, suffix);
	}
	if (stat(override ? override : path, &status) < 0) {
		error = override || errno != ENOENT ? errno : 0;
		goto done;
	}
	if (!S_ISREG(status.st_mode) || status.st_size < 0 ||
	    (uintmax_t)status.st_size > GUI_APPEARANCE_MAX_BYTES) {
		error = EINVAL;
		goto done;
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
	    (uintmax_t)status.st_size > GUI_APPEARANCE_MAX_BYTES) {
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
	gui_appearance_parse(appearance, contents, bytes);
done:
	if (error)
		fprintf(stderr, "mario-gui: desktop config unavailable: %s: %s; using defaults\n",
			override ? override : path, strerror(error));
	if (descriptor >= 0)
		close(descriptor);
	free(contents);
	free(path);
}
