# Desktop Appearance

The desktop and window manager read the same configuration at startup:

1. `MARIO_DESKTOP_CONFIG`, if set, selects a guest file. An empty value disables
   configuration and keeps the compiled-in defaults.
2. `$XDG_CONFIG_HOME/mario/desktop.conf`, when `XDG_CONFIG_HOME` is nonempty.
3. `$HOME/.config/mario/desktop.conf` otherwise.

An editable default file is bundled at `/root/.config/mario/desktop.conf`.
It preserves the bitmap font, original colors, and optional default wallpaper.
`tests/gui/default-appearance.test` covers the init-managed session with these
files applied, including full-screen desktop and terminal screenshot baselines.
For example, edit it inside Mario:

```ini
font_family=DejaVu Sans
font_size=20
color_scheme=light
foreground=#111827
active=#2563eb
wallpaper=/root/background.png
```

Restart the whole GUI session, not just a terminal, to apply changes. For a
manual session, set `MARIO_DESKTOP_CONFIG=/root/custom-desktop.conf` before
starting `/usr/bin/mario-wm`; the shell inherits the same selection. To install
a persistent configuration from the host, edit
`rd/_ramdisk0/root/.config/mario/desktop.conf` and rebuild the disk image.

## Fonts

`font_family` accepts the bundled `DejaVu Sans`, `DejaVu Sans Bold`,
`DejaVu Sans Mono`, and `DejaVu Sans Mono Bold` families. `font_size` is a pixel
height from 8 through 32. Specifying a size without a family selects DejaVu Sans.
For another installed font, use an absolute guest path:

```ini
font_path=/root/fonts/MyFont.ttf
font_size=18
```

The last valid `font_family` or `font_path` setting selects the font. These
settings affect the launcher, menu, taskbar labels, and clock. Panel height,
button sizes, reserved clock space, menu width, hit testing, and the WM work
area follow font metrics. Text is clipped to its button and rendered into a
reusable antialiased cache, not rasterized for each screen pixel.

With neither font setting, the original fixed 10-pixel bitmap font and layout
remain unchanged. `font_family=bitmap` explicitly selects that legacy font;
its size is fixed, so `font_size` does not scale it. A missing or invalid font
falls back to the bitmap font without preventing GUI startup.

These settings do not change terminal or application fonts. Window decorations
use the configured colors but retain their existing geometry and icon rendering.

## Colors

`color_scheme` accepts `default`, `dark`, `light`, and `solarized-dark`.
`dark` is an alias for the original default theme. Optional `#RRGGBB` overrides:

| Key | Applies to | Default |
| --- | --- | --- |
| `background` | Solid desktop background | `#1f2933` |
| `panel` | Panel background | `#111827` |
| `active` | Launcher, menu, focused task button, active title bar | `#2563eb` |
| `inactive` | Inactive task buttons and title bars | `#475569` |
| `foreground` | Desktop text and navigation arrows | `#f8fafc` |
| `error` | Launcher failure indicator | `#ef4444` |
| `border` | Window borders and normal title-button icons | `#0f172a` |
| `content` | Empty window content and hovered title buttons | `#f8fafc` |

Explicit colors override the scheme regardless of their order in the file.
The last valid repeated setting wins. Wallpaper overrides only the solid
desktop background. Terminal colors remain controlled by `terminal.conf`.

## Wallpaper

Set `wallpaper` to an absolute guest PNG/BMP path, or leave its value empty
(`wallpaper=`) to disable wallpaper. The path is limited to 255 bytes. The
desktop loads the image once and scales it to cover the display while preserving
its aspect ratio and center-cropping. The panel and menu remain on top.

`MARIO_WALLPAPER`, when set, takes precedence over the configuration file; an
empty environment value disables wallpaper regardless of the file. Otherwise,
the configured path is used, defaulting to `/usr/share/mario/wallpaper.png`.
No image is bundled at that path. A missing default image silently uses the
solid background; other loading failures are reported and use the same fallback.
See [Desktop Wallpaper](desktop.md) for supported image formats and limits.

## Parsing And Fallbacks

All settings are optional. Whitespace around keys and values, blank lines, and
whole-line `#` comments are accepted. Invalid entries are reported and ignored;
the other valid settings still apply. Missing default configuration is silent.
An unavailable explicit file falls back to defaults.

Configuration must be a regular file no larger than 64 KiB. Lines are limited
to 287 bytes, and font paths to 255 bytes. Nonregular files, including FIFOs,
are rejected without waiting for a writer. Live reload and graphical appearance
settings are not implemented; restart the GUI session after editing the file.
