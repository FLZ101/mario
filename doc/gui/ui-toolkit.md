# `libmario-ui`

Part of the [GUI platform plan](README.md). This document specifies the
userspace drawing and widget library shared by the desktop and applications.

## Scope and Authority

This is the normative contract for `libmario-ui` public types, rendering,
damage, text, image, and event-loop behavior. It consumes the kernel ABI from
[Kernel foundations](kernel-foundations.md) and supplies the desktop/application
contracts in [Desktop and applications](desktop.md). It does not set milestone
order, current status, or commit boundaries; those belong exclusively to the
[implementation roadmap](roadmap.md).

### Layering Rules

- `libmario-ui` owns in-memory surfaces, clipping, damage, text, image decode,
  and event dispatch; it does not own foreground-console policy or mode
  switching.
- From M3 onward only `mario-wm` opens `/dev/fb0` and raw-input devices. The
  shell and every application render into toolkit surfaces and submit bounded
  updates through the window-manager protocol.
- Client and server use the same surface/damage vocabulary, but only the
  server converts the final composition to the framebuffer format and flushes
  scanout.
- Toolkit unit tests validate pure surface, font, decoder, and event behavior.
  GUI tests validate a visible integration through the desktop; toolkit code
  does not define test-runner directives or QMP behavior.

Create a static userspace library under:

```text
app/gui/libmario-ui/
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
    gui_rect bounds;
    int full;
} gui_region;

typedef struct gui_font gui_font;

typedef enum {
    GUI_EVENT_NONE,
    GUI_EVENT_KEY_DOWN,
    GUI_EVENT_KEY_UP,
    GUI_EVENT_MOUSE_MOVE,
    GUI_EVENT_MOUSE_DOWN,
    GUI_EVENT_MOUSE_UP,
    GUI_EVENT_WINDOW_CLOSE_REQUEST,
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
For all userspace budgets, `gui_frame_bytes` means checked
`framebuffer_width * framebuffer_height * sizeof(gui_color)`, regardless of
the physical scanout's 16/24/32-bpp byte size.

Surface and geometry safety rules:

- `gui_surface_create()` rejects nonpositive dimensions and any `width *
  height * sizeof(gui_color)` overflow before allocation. The caller supplies
  only dimensions already accepted by the desktop's scanout-based memory
  budget; standalone tools use the same checked limit rather than allocating
  from untrusted image dimensions.
- Clipping computes rectangle endpoints in a wider signed type before clamping
  to the destination. It must not evaluate `x + w` or `y + h` in a way that can
  wrap a signed `int` supplied by a malformed image or protocol message.
- A damage region holds at most 64 rectangles. Insertions coalesce overlapping
  or adjacent rectangles; if it is still full, replace the region with one
  full-surface rectangle. Damage tracking is therefore bounded even while a
  client or application redraws many tiny areas.

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
void         gui_region_init(gui_region *region, gui_region_rect *rects,
                             int capacity, gui_rect bounds);
void         gui_region_add(gui_region *region, gui_rect r);
void         gui_region_clear(gui_region *region);
gui_flush_result gui_surface_flush_rect(const gui_surface *s, gui_rect r,
                                        size_t scanout_stride,
                                        gui_pwrite64_fn pwrite64_fn,
                                        void *context);
gui_flush_result gui_surface_flush_rect_fd(const gui_surface *s, gui_rect r,
                                           size_t scanout_stride, int fd);
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
  list with the full region bounds and sets `full`, ensuring damage tracking
  remains bounded. The caller supplies those bounds at initialization.
- `gui_surface_flush_rect()` clips a single rectangle and writes every row with
  positional I/O. It retries short writes and `EINTR`; it returns
  `GUI_FLUSH_BUSY` for `EBUSY` without changing damage, so the caller retains
  and retries the complete original rectangle after the foreground transition.
  Other I/O errors return `GUI_FLUSH_ERROR`.
- The compositor never reads damage regions from an app-owned surface after
  the app has modified it without taking the window lock; a bounded
  `SURFACE_BEGIN`/`SURFACE_DATA`/`SURFACE_COMMIT` transaction transfers each
  client update, so there is no shared-memory race.
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
- The window manager limits total live window-surface bytes to 2.5 times
  `gui_frame_bytes` initially. A create or resize that would exceed the limit
  fails and leaves the existing window geometry unchanged; tiled/partial
  backing stores are deferred.
- Surfaces are explicitly freed on window close and on ordinary-client
  disconnect. On shell disconnect the server retains only the last committed
  desktop surface as an input-inert snapshot until a replacement shell commits
  its desktop surface or the session ends; every other shell surface is freed.
  There is no garbage collector or reference-counted surface API.
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

- Keep the bundled DejaVu sources under `app/gui/libmario-ui/assets/fonts/`
  and install them under `rd/usr/share/fonts/` in the common image. DejaVu Sans is
  the proportional desktop font; DejaVu Sans Mono is used for the terminal and
  file-manager columns. `FONT-VERSION.txt` records the upstream version and
  SHA-256 of every bundled font file; a changed hash requires regenerating and
  reviewing font goldens.
- `gui_font_load(path)` calls `FT_New_Memory_Face()` on the file contents and
  selects an integer pixel height with `FT_Set_Pixel_Sizes()`, never a
  host-DPI-dependent point size. `gui_font` owns that file buffer and frees it
  only after `FT_Done_Face()`. Reject a font file larger than 8 MiB before
  reading it into memory.

Glyph rendering:

1. Convert a UTF-8 sequence to a Unicode codepoint.
2. Look up the codepoint in the glyph cache.
3. Rasterize a miss as 8-bit alpha using `FT_Load_Glyph()` and
   `FT_Render_Glyph()` with normal grayscale rendering, no hinting, no subpixel
   positioning, and no LCD filter.
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
    uint16_t width, height, stride;
    uint8_t *alpha;
} gui_glyph_bitmap;

typedef struct {
    uint32_t font_id;
    uint32_t codepoint;
    uint16_t pixel_height;
    uint8_t bold, italic;
    gui_glyph_bitmap bitmap;
    int advance;
    int x_off, y_off;
} gui_glyph_cache;
```

The cache key includes `font_id` so proportional and monospace faces cannot
alias the same codepoint/size entry. Each entry owns its 8-bit alpha allocation;
LRU eviction frees that allocation before reusing the entry.

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
framebuffer directly; the `mario-wm` compositor copies dirty rectangles to
`/dev/fb0`.

`mario-desktop` also owns one full-scanout, non-decorated desktop surface. The
window manager accepts that surface only on its private shell-capability
connection and always composites it below normal windows; it is a protocol role,
not a separate rendering path.

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

`mario-wm` uses `poll()` on:

- `/dev/fb0` for `POLLPRI` foreground-transition notifications;
- `/dev/gui` for fixed-size `struct mario_gui_key_event` records;
- `/dev/psaux` for fixed-size `struct mario_psaux_event` records;
- client socket fds.

The shell polls its private connected socket; ordinary applications connect to
the public pathname listener. Both use the same framing and surface API, but
only the shell receives window-list notifications and may update the desktop
surface.

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
- Tick values are 32-bit and wrap. Delays and auto-repeat use unsigned modular
  subtraction, never an absolute comparison or sort across the wrap boundary.
  The loop preserves the order records were read from each fd; timestamps are
  diagnostic/repeat inputs, not permission to reorder already queued events
  from distinct device streams.
- The window manager owns the physical pressed-key/button state. On focus
  change, client disconnect, graphics-to-text switch, an input-overflow record,
  or cancellation of a move/resize grab, it ends the grab and sends one
  `INPUT_RESET` to the former focused client before routing later input. Clients
  clear modifier, repeat, drag, and selection state on that event; they must
  not wait for synthetic key-up records that the kernel cannot guarantee.
- The initial server and clients use no app threads; each event loop runs on
  one thread. Long-running work such as image decode is incremental and yields
  between bounded decode chunks; the first GUI has no worker threads.
- Mouse events are delivered to the window under the pointer unless a button
  press starts a move/resize or popup grab. During a grab, all mouse events go
  to the grab owner until release; keyboard focus is unchanged.
- A window receives keyboard events only when it has focus. The window manager
  intercepts global shortcuts before routing to the focused window.
- Socket messages are length-prefixed and enqueued as events. The server
  processes each message fully before polling again, which keeps client
  operations atomic without locks between processes.

## Image Decoding

Use small internal code for BMP and libpng for the only compressed format in
the first milestone:

- BMP: uncompressed 24/32 bpp, bottom-up or top-down;
- PNG: 8-bit RGB/RGBA through libpng and zlib;

Decoded images are converted to `gui_surface` and cached by the image viewer.
If decoding would create more pixels than one framebuffer, the decoder
downscales before allocating the surface; the original image is not retained.
The BMP and PNG header paths validate width, height, row stride, and total
decoded bytes with checked 64-bit arithmetic before allocating or asking a
library to inflate data. Truncated, malformed, or over-budget images return a
normal error to the viewer; they never leave a partially initialized surface in
the cache. The decoded destination, temporary row buffers, and glyph/image
caches remain charged to the existing GUI memory budget.
- BMP accepts only the documented uncompressed 24/32-bpp headers, requires the
  file length to cover every checked source row, and rejects source dimensions
  above 4096 by 4096. PNG calls `png_set_user_limits(4096, 4096)` before reading
  image information, accepts only non-interlaced 8-bit RGB/RGBA data after
  explicit format checks, and decodes scanline-by-scanline into the bounded
  downscaler rather than calling a whole-image libpng helper. A libpng error
  unwinds the current row/destination allocation before returning failure.
The image viewer keeps at most two decoded full-image surfaces and evicts the
least-recently-used one before decoding another; thumbnails are separately
bounded and may be regenerated from the source file.

Vendor `libpng` under `app/usr/libpng/`. Add `build-libpng.sh`/`libpng.mk`
matching the existing app build style. The image viewer links it statically.
JPEG is a later additive decoder and does not block the first image-viewer
milestone.
