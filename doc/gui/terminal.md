# Terminal Configuration

The terminal reads configuration once at startup. Existing defaults remain a
14-pixel DejaVu Sans Mono font, the original dark palette, and opaque content.
The default 8 × 16 cell geometry stays unchanged for existing applications.

Configuration paths, in priority order:

1. `MARIO_TERMINAL_CONFIG`, when set, names a guest file. An empty value disables
   configuration and uses defaults.
2. `$XDG_CONFIG_HOME/mario/terminal.conf`, when `XDG_CONFIG_HOME` is nonempty.
3. `$HOME/.config/mario/terminal.conf` otherwise.

For example, create `/root/.config/mario/terminal.conf` in the guest:

```ini
font_size=20
color_scheme=dark
foreground=#f8fafc
background=#111827
cursor=#facc15
opacity=85
color1=#ef4444
```

All settings are optional. Whitespace around keys and values is ignored;
blank lines and whole-line `#` comments are accepted. Invalid settings are
reported and ignored, while valid settings still take effect. Files must be
regular files no larger than 64 KiB; lines are limited to 255 bytes. Missing
default configuration is silent. An unavailable explicitly selected file
falls back to defaults without preventing terminal startup.

| Setting | Values | Default |
| --- | --- | --- |
| `font_size` | Integer pixel height, 8–48 | `14` |
| `color_scheme` | `default`, `dark`, `light`, `solarized-dark` | `default` |
| `foreground` | `#RRGGBB` | `#f8fafc` |
| `background` | `#RRGGBB` | `#111827` |
| `cursor` | `#RRGGBB` | `#f8fafc` |
| `color0` … `color15` | ANSI palette entries, `#RRGGBB` | Original palette |
| `opacity` | Integer percentage, 0–100 | `100` |
| `opaque` | `true`/`1` for 100%; `false`/`0` for 80% | `true` |

Explicit color overrides apply after the selected scheme, regardless of where
they appear in the file. The last valid repeated setting wins. `opaque` is a
convenience alias: if combined with `opacity`, the last of those settings wins.
ANSI indexed colors use the configured palette; true-color escape sequences
continue to use the RGB values supplied by applications.

For non-default font sizes, character advance and font metrics determine the
cell size and baseline. The resulting grid is reported to the PTY through
`TIOCSWINSZ`, including later window resizes and fullscreen changes. Existing
cursor-shape escapes and blinking continue to use the configured cell geometry.

Opacity applies to the complete terminal content, including text and cursor;
title bars and resize borders remain opaque. The WM blends content with the
wallpaper and lower ordinary windows, and preserves their updates through
transparent windows. A fullscreen terminal blends with the desktop rather than
other ordinary windows. Older WMs without opacity support render opaque content
and emit a warning instead of disconnecting the terminal.

To install a persistent configuration, place it at
`rd/_ramdisk0/root/.config/mario/terminal.conf` on the host and rebuild the disk
image. No configuration is bundled by default. Restart the terminal to apply
changes; live configuration reload and a graphical settings editor are not
implemented.

The protocol extension is negotiated with `GUI_PROTOCOL_CAP_WINDOW_OPACITY`
(`0x00000002`). `GUI_PROTOCOL_WINDOW_OPACITY` carries two little-endian
32-bit fields: the client's own ordinary window ID and an opacity percentage.
The WM rejects unnegotiated requests, other clients' windows, shell windows,
invalid lengths, and percentages above 100. Existing XRGB surface uploads and
their patch negotiation remain unchanged.
