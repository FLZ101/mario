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
void         gui_region_mark_full(gui_region *region);
gui_flush_result gui_surface_flush_rect(const gui_surface *s, gui_rect r,
                                        size_t scanout_stride,
                                        gui_pwrite64_fn pwrite64_fn,
                                        void *context);
gui_flush_result gui_surface_flush_rect_fd(const gui_surface *s, gui_rect r,
                                           size_t scanout_stride, int fd);
gui_flush_result gui_surface_flush_region(const gui_surface *s,
                                          gui_region *region,
                                          size_t scanout_stride,
                                          gui_pwrite64_fn pwrite64_fn,
                                          void *context);
gui_flush_result gui_surface_flush_region_fd(const gui_surface *s,
                                             gui_region *region,
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
- `gui_surface_flush_region()` flushes rectangles in their stored order and
  removes only those which complete. A compositor calls `gui_region_mark_full()`
  on every `/dev/fb0` `POLLPRI` transition: the full scanout must be redrawn
  because an earlier successful write can have been superseded by the change.
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

### Surface Memory and Failure Handling

- `gui_surface_create()` returns `NULL` on allocation failure. Callers must
  show an in-band error status or close the window; they must never dereference
  a failed surface.
- Through M8 the window manager limited live window-surface bytes to 2.5 times
  `gui_frame_bytes`. M9.1 replaces this with the checked
  `min(16 * gui_frame_bytes, 64 MiB)` session limit. This includes all committed
  surfaces, staging surfaces, retained desktop snapshots, and outstanding
  create/resize reservations, without counting the same allocation twice.
  The compositor's single scanout-sized scratch surface and client-side
  rendering allocations are outside this pool and must be accounted for
  separately; the limit is not a bound on total GUI-process memory.
- Under M9.1, ordinary clients cannot consume the one-frame headroom needed to
  stage a desktop replacement. That headroom is part of the session limit,
  not an additional allowance; an existing shell staging allocation satisfies
  the reservation. Public clients never acquire shell privileges through
  window-count or surface-budget pressure.
  A scanout requiring more than half the capped pool is refused at startup,
  since it cannot preserve both the desktop and its replacement.
- From M9.2, each ordinary connection has an aggregate
  surface/reservation allowance of `min(8 * gui_frame_bytes, 32 MiB)` across
  all its windows, still subject to the session cap. Each window retains its
  own one-frame surface limit and one in-flight staging transaction. Opening
  another window on the same connection cannot reset the peer's accounting;
  splitting a workload across connections cannot bypass the session limit.
- Dynamic client, window, pending-launch, and poll records are not unbounded:
  charge actual record/table capacity to a 1 MiB WM metadata budget and a
  256 KiB per-connection share. The shell's dynamic window/snapshot model has
  its own 1 MiB metadata budget. These budgets are separate from surfaces and
  the existing independent 64 KiB input/output queues per endpoint. Check
  allocation growth and arithmetic before accepting new records; release
  unused storage on teardown. No fixed-size array or numeric window-count cap
  substitutes for this accounting. Kernel fd/process limits remain additional
  admission constraints, not reasons to terminate existing peers.
  M9.2 allocates these records dynamically, retains connection-scoped
  protocol queues, and charges a logical poll slot to each peer while
  charging actual poll capacity once globally. Reaping frees launch records;
  disconnect detaches their peer charge without losing the global charge
  for a child still being tracked. Freed windows do not reuse identities.
- A create, resize, or fullscreen transition must reserve its required
  capacity before being accepted. Quota exhaustion returns a normal protocol
  error, preserves existing geometry and committed pixels, and leaves other
  clients usable. Allocation or commit failure during an accepted transition
  restores the prior geometry and mode; any corrective configure uses a new
  serial, never a reused or decremented serial. Destroy, disconnect, canceled
  configure, and failed transactions release all associated reservations.
  M9 retains atomic full-surface transactions and checked per-surface sizes
  no larger than one scanout; tiled/partial backing stores remain deferred.
  M9.1 charges a reservation at create/resize admission and converts it into
  staging storage without a second charge. Superseding a resize frees its
  staging/reservation only after the new reservation fits; a refused request
  does not cancel the existing transaction. Rollback restores the geometry
  preceding the first uncommitted resize, frees the failed transaction, and
  sends a fresh configure. If the first frame of an accepted create fails,
  its empty window retains its identity and geometry for a later repaint,
  without holding a reservation. A suspended scanout (`GUI_FLUSH_BUSY`) is
  still a successful logical commit, not a rollback.
  Terminal, image viewer, file manager, desktop, and diagnostics defer
  recoverable surface errors to a 100 ms timer-driven retry instead of
  disconnecting or spinning; queued input does not restart the timer.
  They drain the one abandoned chunk reply and any preceding
  stale-begin error before starting another frame; unrelated protocol errors
  remain fatal. Corrective configures are strictly monotonic.
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

Text layout is independent of glyph rasterization. `gui_text_next()` decodes
one NUL-terminated UTF-8 sequence and identifies malformed input as the CP437
`?` fallback. `gui_text_measure()` obtains advances from a font callback, uses
the fixed 8-pixel CP437 advance when the callback has no glyph, and reports a
single-line width, baseline, and line height. Its input metrics use a positive
ascent, a nonpositive descent, and a nonnegative line gap; the baseline is the
ascent and the line height is `ascent - descent + line_gap`.

```c
int gui_text_next(const char **text, gui_text_glyph *glyph);
int gui_text_measure(const char *text, const gui_font_metrics *metrics,
                     gui_text_lookup_fn lookup, void *context,
                     gui_text_layout *layout);
```

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

`gui_font_load(path, pixel_height)` owns the bounded font file buffer and one
FreeType face. A `gui_font` is one `(font_id, pixel_height)` cache namespace;
it holds at most 2048 grayscale entries and evicts the least recently used
entry, breaking an age tie by the lower cache slot. `gui_font_get_glyph()`
rasterizes with `FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP` and
`FT_RENDER_MODE_NORMAL`. `gui_font_draw_glyph()` applies an opaque XRGB theme
color as full alpha when its high byte is zero, or uses a supplied ARGB alpha.
`gui_font_draw_text()` draws a fixed 8x16 CP437 `?` for malformed UTF-8 or a
font-missing codepoint before advancing by the same fixed fallback width.

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

### Incremental Presentation and Uploads (M10 Target)

These are planned contracts; through M9 the wire protocol accepts only complete
configured-size surface replacements. Existing committed window surfaces and
the bounded damage/flush helpers are reused rather than replaced by a new
rendering stack or an assumed GPU API.

M10.4 batches a full-width rectangle into a contiguous span only when its
source stride and scanout pitch both equal the intended row bytes. Narrow
rectangles and padding on either side retain scanline writes. Requests are
capped at `SSIZE_MAX`, without a packing buffer; partial writes can cross row
boundaries and retry at the exact byte offset. BUSY retains the original
unfinished rectangle, and region retries preserve order. The WM's native build
now depends on the static toolkit archive, preventing stale scanout code after
library-only edits. The complete 1024x768 fixture uses one 3145728-byte writer
call; its 320x192 content update remains 192 narrow scanline calls.

- The WM owns screen-space damage. Application cell/region tracking and WM
  damage are separate: changing geometry exposes cached pixels without asking
  the application to rerender them. Clip fill/blit operations as well as
  scanout, preserve region ordering on retries, and retain the bounded
  full-region fallback. Damage tracking must not allocate per input event.
- A contiguous flush fast path is valid only when source row layout and
  framebuffer stride make all bytes in the span both contiguous and intended
  for writing. Otherwise retain scanline writes. Check offsets, lengths, and
  signed I/O limits; short writes, `EINTR`, `EBUSY`, format/layout constraints,
  and foreground-generation checks keep their existing meanings. No
  framebuffer mmap, new ioctl, or generation-check bypass is implied.
- M10's rectangular-update extension requires explicit version/capability
  negotiation before use; v1.0 full-frame clients remain supported. Do not
  silently reinterpret their `SURFACE_BEGIN` payload. Before implementation,
  specify the negotiated payloads and error results in the protocol header
  and protocol tests. One patch transaction targets one in-bounds rectangle
  and identifies its window, current configure serial, exact committed base
  frame, and new frame serial. Reject stale bases, invalid bounds, overflow,
  duplicate/out-of-order/incomplete data, and incompatible geometry without
  exposing partial pixels or discarding the prior committed surface.
- Keep committed surfaces immutable while staging a patch. An initial safe
  implementation reserves and charges a full-size staging copy of the base,
  writes the patch into that copy, and atomically swaps on complete commit.
  Charge every retained, staging, and reserved byte under the M9 peer/session
  budgets. Patch admission must not consume shell headroom or evade a
  configured-size limit. Quota/allocation failure, cancellation, disconnect,
  and superseded configure reclaim staging and preserve known-good content.
  Creation, resize, fullscreen transition, and lost-base recovery use a full
  replacement before further patches; partial/tiled backing stores remain
  out of scope.
- The terminal's first pipelined uploader uses a documented 16 KiB encoded
  unacknowledged-data window, counting message headers, and independently
  caps reply and PTY buffers. Keep one surface transaction per window and
  match each existing `SURFACE_DATA_REPLY` to its frame/configure/chunk.
  Handle short writes and `EAGAIN` with poll-driven progress, process stale
  replies during cancellation without applying them to a newer transaction,
  and retain recoverable-error backoff. No busy waits, additional commit
  acknowledgements, or larger protocol queue quotas are required.

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

`gui_input_normalize_key_records()` and
`gui_input_normalize_mouse_records()` copy raw device records into ordered
`gui_input_event` values. A marked overflow first produces the corresponding
key or mouse reset with the marked record's tick, then the copied record. This
normalization preserves raw scancodes, relative mouse fields, button masks,
and ticks; keysym translation, pointer coordinates, button-edge detection,
and focused-window policy remain above this layer.

Events are timestamped, queued, and dispatched to the focused window or the
window manager. `poll()` uses a 16 ms timeout; every timeout produces one
`GUI_EVENT_TIMER`. The first GUI does not require a timer fd, signal, or extra
pipe.

`gui_event_loop` owns generic `poll()` registrations and one-shot monotonic
millisecond deadlines. `gui_event_loop_run_once()` snapshots ready fds, invokes
their callbacks in registration order, then invokes due timers in registration
order. Removing a handle suppresses any later callback from that same wake;
sources registered by a callback begin on the next wake. These rules keep a
callback free to remove itself or a later source without invalidating dispatch.

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
