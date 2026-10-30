# `libmario-ui`

Part of the [GUI platform plan](README.md). This document specifies the
userspace drawing and widget library shared by the desktop and applications.


Create a static userspace library under:

```text
app/usr/libmario-ui/
  include/gui/gui.h
  include/gui/surface.h
  include/gui/event.h
  include/gui/widget.h
  src/surface.c
  src/font.c
  src/image.c
  src/event.c
  src/widget.c
```

The library deliberately has no kernel-specific dependencies beyond libc file
operations, `poll`, and memory allocation.

## Core Types

```c
typedef struct { int x, y, w, h; } gui_rect;
typedef uint32_t gui_color; /* premultiplied ARGB: 0xAARRGGBB */

typedef struct {
    int width;
    int height;
    int stride; /* gui_color elements, always equal to width initially */
    gui_color *pixels;
} gui_surface;

typedef struct {
    int x, y;
    int width, height;
} gui_region_rect;

typedef struct {
    int count;
    int capacity;
    gui_region_rect *rects;
} gui_region;

typedef struct gui_font gui_font;

typedef enum {
    GUI_EVENT_NONE,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_WINDOW_CLOSE,
    GUI_EVENT_WINDOW_RESIZE,
    GUI_EVENT_TIMER
} gui_event_type;

typedef struct {
    uint32_t ticks;
    gui_event_type type;
    uint32_t window_id;
    union {
        struct { uint32_t keysym; uint32_t modifiers; } key;
        struct { int x, y; uint8_t buttons; } mouse;
        struct { int width, height; } resize;
    };
} gui_event;
```

`ticks` is the monotonic kernel tick copied from the input device event; the
toolkit does not convert it to wall-clock milliseconds. Printable keysyms are
Unicode codepoints. Non-printable keysyms start at `0x110000` and initially
cover arrows, Home/End, Page Up/Down, Insert/Delete, and Escape. Modifier bits
are `GUI_MOD_SHIFT` (`1 << 0`), `GUI_MOD_CTRL` (`1 << 1`), `GUI_MOD_ALT`
(`1 << 2`), `GUI_MOD_SUPER` (`1 << 3`), and `GUI_MOD_CAPS_LOCK` (`1 << 4`).
The initial keyboard map is US QWERTY set-1; compose, layouts, and IMEs are
out of scope. Function keys have no `keysym` because the kernel reserves them.

## Drawing Primitives

Initial implementation:

- fill rectangle with solid color;
- copy/blit one surface into another with clipping;
- alpha blend one surface over another;
- draw horizontal/vertical lines;
- draw 1-pixel rounded rectangles for window chrome;
- draw text using the TTF glyph cache with an embedded 8x16 CP437 fallback;
- set a damage rectangle after every draw operation.

The scanout format is `XRGB8888` for 32 bpp. Internal surfaces are always
premultiplied `ARGB8888`; if the fallback scanout mode is 24 or 16 bpp,
`libmario-ui` composites first and converts only while flushing.
Surfaces are tightly packed row-major allocations: `stride == width`, and no
external, sub-surface, or padded-stride API exists in the first milestone.

Public surface API:

```c
gui_surface *gui_surface_create(int w, int h);
void         gui_surface_destroy(gui_surface *s);
void         gui_fill_rect(gui_surface *s, gui_rect r, gui_color c);
void         gui_blit(gui_surface *dst, int dx, int dy,
                      const gui_surface *src, const gui_rect *src_rect);
void         gui_blend(gui_surface *dst, int dx, int dy,
                       const gui_surface *src, const gui_rect *src_rect,
                       uint8_t global_alpha);
void         gui_line_h(gui_surface *s, int x, int y, int w, gui_color c);
void         gui_line_v(gui_surface *s, int x, int y, int h, gui_color c);
void         gui_draw_rect(gui_surface *s, gui_rect r, gui_color c);
void         gui_draw_text(gui_surface *s, int x, int y,
                           gui_font *font, const char *utf8, gui_color fg);
void         gui_region_add(gui_region *region, gui_rect r);
void         gui_region_clear(gui_region *region);
```

Clipping and damage rules:

- Every primitive clips to the destination surface and returns immediately if
  the clipped rectangle is empty.
- `gui_fill_rect`, `gui_blit`, `gui_blend`, and `gui_draw_text` append the
  affected destination rectangle to the owning window's damage region.
- Source and destination rectangles must not overlap when `gui_blit()` or
  `gui_blend()` receives the same surface; callers needing that operation use
  a temporary surface.
- Damage regions are merged before compositing. Overlapping rectangles may be
  coalesced if their union does not exceed twice the sum of their areas.
- A region holds at most 64 rectangles. Adding a 65th rectangle replaces the
  list with the clipped bounding rectangle, ensuring damage tracking remains
  bounded.
- The compositor never reads damage regions from an app-owned surface after
  the app has modified it without taking the window lock; in Phase 2 the
  damage list travels with `SURFACE_UPDATE`, so there is no shared-memory race.
- Alpha blending operates in premultiplied `ARGB8888`. The final flush blends
  window pixels against the opaque desktop, drops alpha, then writes the RGB
  result expected by the framebuffer.

Pixel conversion:

- `gui_surface` is always premultiplied `ARGB8888`. The flush path converts
  its composited RGB result to the `/dev/fb0` format with a small per-bpp
  conversion table or inline function.
- For RGB565, use 5/6/5 truncation with round-to-nearest. For packed RGB888,
  write bytes in memory order rather than assuming endianness.
- The `FBIOGET_*` mask values are used to build the conversion table once at
  startup; no hardcoded host-endian assumptions remain in the userspace code.

Surface memory and failure handling:

- `gui_surface_create()` returns `NULL` on allocation failure. Callers must
  show an in-band error status or close the window; they must never dereference
  a failed surface.
- The window manager limits total live window pixels to 2.5 times the
  framebuffer size initially. A create or resize that would exceed the limit
  fails and leaves the existing window geometry unchanged; tiled/partial
  backing stores are deferred.
- Surfaces are explicitly freed on window close and on client disconnect.
  There is no garbage collector or reference-counted surface API in Phase 1.
- Temporary scratch buffers used by image decode and glyph rasterization are
  capped: image decode uses one 4 MiB scratch buffer, and font glyph cache
  uses an LRU limit of 2048 entries.

## TrueType Font Rendering

Use FreeType as the primary TTF/OTF backend. Vendor it under
`app/usr/freetype/` and build it as a static library with
`util/mario-musl-cc`, following the existing `build-zlib.sh`/`zlib.mk`
pattern.

FreeType build notes:

- Disable unused modules: no auto-hinter needed initially, no Type 1/CID, no
  bitmap strike, no OpenType validation, no subpixel LCD filtering.
- Enable the SFNT, TrueType, CFF, and basic outline modules needed by common
  `.ttf` and `.otf` files.
- Keep `FT_CONFIG_OPTION_USE_PNG` off unless image fonts are required.

Font loading:

- Keep the bundled DejaVu sources under `app/usr/libmario-ui/assets/fonts/`
  and install them under `rd/usr/share/fonts/` in the common image. DejaVu Sans is
  the proportional desktop font; DejaVu Sans Mono is used for the terminal and
  file-manager columns.
- `gui_font_load(path)` calls `FT_New_Memory_Face()` on the file contents and
  selects a point size. `gui_font` owns that file buffer and frees it only
  after `FT_Done_Face()`.

Glyph rendering:

1. Convert a UTF-8 sequence to a Unicode codepoint.
2. Look up the codepoint in the glyph cache.
3. Rasterize a miss as 8-bit alpha using `FT_Load_Glyph()` and
   `FT_Render_Glyph()`.
4. Convert alpha to a `gui_color` while drawing, so one glyph cache can render
   in any foreground color.
5. Apply horizontal advance and kerning from `FT_Get_Advance()` and
   `FT_Get_Kerning()`.
6. Fall back to the embedded CP437 glyph when the TTF does not contain the
   requested codepoint.

Malformed UTF-8 consumes one byte and renders the CP437 fallback glyph; it
never reads beyond the supplied NUL-terminated string.

Font cache:

```c
typedef struct {
    int codepoint;
    int size;
    int bold;
    int italic;
    gui_surface glyph;
    int advance;
    int x_off, y_off;
} gui_glyph_cache;
```

Font metrics include ascent, descent, line gap, and underline position so the
terminal can compute row height and the window manager can size title bars
correctly.

Initial supported scripts:

- ASCII and Latin-1;
- box-drawing and block characters used by terminal applications;
- common punctuation and symbols;
- synthetic bold/italic by horizontal offset and shear if the bundled font
  lacks dedicated variants.

Complex text shaping, bidirectional text, and color emoji are out of scope for
the first TTF milestone. FreeType rasterizes individual glyphs; full shaping
would require a separate HarfBuzz/Pango layer later. Isolate
codepoint-to-glyph rasterization in `src/font.c` so that layer can be added
without a kernel change.

## Window Surfaces

Each window has a backing `gui_surface`. Drawing to a window never touches the
framebuffer directly; the compositor copies dirty rectangles to `/dev/fb0`.

Window properties:

- position and z-order;
- title string;
- minimum and maximum size;
- focus state;
- dirty rectangle list;
- optional icon/pixmap.

## Widgets

Implement only the widgets needed by the first applications:

- `gui_window`: top-level window with title bar and close button;
- `gui_panel`: horizontal or vertical panel;
- `gui_button`: clickable labeled button;
- `gui_label`: static text;
- `gui_textbox`: single-line editable text;
- `gui_list`: vertical list with scrolling;
- `gui_scrollbar`;
- `gui_terminal`: terminal grid surface used by the terminal app;
- `gui_image`: decoded image with fit/zoom/pan state;
- `gui_file_icon`: file-manager icon or list item.

## Event Loop

The desktop uses `poll()` on:

- `/dev/gui` for fixed-size `gui_key_event` records;
- `/dev/psaux` for fixed-size `psaux_event` records;
- application socket fds in the multi-process model.

Events are timestamped, queued, and dispatched to the focused window or the
window manager. `poll()` uses a 16 ms timeout; every timeout produces one
`GUI_EVENT_TIMER`. The first GUI does not require a timer fd, signal, or extra
pipe.

On a `GUI_KEY_EVENT_OVERFLOW` or `PSAUX_EVENT_OVERFLOW` record, the event loop
clears its corresponding pressed-key or mouse-button state before dispatching
the record. On `/dev/fb0` `POLLPRI`, it clears both states and discards any
coalesced pointer movement before processing subsequent input.

Event ordering and concurrency:

- Input uses the monotonic tick captured in each kernel event. Ties are broken
  by fd order: keyboard, mouse, timer, then GUI socket.
- Phase 1 uses no app threads for the initial shell; the event loop, layout,
  and compositing run on one thread. Long-running work such as image decode is
  incremental and yields to the event loop between bounded decode chunks; the
  first GUI has no worker threads.
- Mouse events are delivered to the window under the pointer unless a button
  press starts a move/resize or popup grab. During a grab, all mouse events go
  to the grab owner until release; keyboard focus is unchanged.
- A window receives keyboard events only when it has focus. The window manager
  intercepts global shortcuts before routing to the focused window.
- In Phase 2, socket messages are length-prefixed and enqueued as events. The
  server processes each message fully before polling again, which keeps client
  operations atomic without locks between processes.

## Image Decoding

Use small internal code for BMP and libpng for the only compressed format in
the first milestone:

- BMP: uncompressed 24/32 bpp, bottom-up or top-down;
- PNG: 8-bit RGB/RGBA through libpng and zlib;

Decoded images are converted to `gui_surface` and cached by the image viewer.
If decoding would create more pixels than one framebuffer, the decoder
downscales before allocating the surface; the original image is not retained.

Vendor `libpng` under `app/usr/libpng/`. Add `build-libpng.sh`/`libpng.mk`
matching the existing app build style. The image viewer links it statically.
JPEG is a later additive decoder and does not block the first image-viewer
milestone.
