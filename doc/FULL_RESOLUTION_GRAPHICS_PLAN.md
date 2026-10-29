# Full Resolution Graphics and GUI Plan

## Goal

Turn Mario from a fixed 80x25 VGA text console into a small but usable
graphical desktop system. The first automated framebuffer target is
1024x768x32; 1920x1080x32 is the later interactive target when supported by
the bootloader and display. The existing text virtual consoles and serial
consoles remain available as fallbacks.

The GUI is not X11 and not Wayland. It is a purpose-built userspace GUI stack
on top of three small kernel services:

1. a linear framebuffer device (`/dev/fb0`);
2. keyboard and mouse input devices;
3. a pseudo-terminal subsystem (`/dev/ptmx` and `/dev/pts/N`).

On top of those services we build:

- a reusable userspace GUI library (`libmario-ui`);
- a desktop shell and window manager;
- a terminal emulator backed by a real PTY;
- an image viewer;
- a file manager;
- a small app launcher/panel and diagnostics surface.

The existing `/dev/tty1` through `/dev/tty6` text consoles are preserved.
Pressing a graphics-console key (initially F8) switches to the GUI, while
F1-F6 continue to switch back to text consoles.

## Success Criteria

Measurable checks, in order of difficulty:

- `qemu-system-i386` with `-vga std` reports and maps a VBE linear framebuffer
  rather than only the legacy text plane.
- `/dev/fb0` reports the real width, height, stride, and depth through
  `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`.
- A lit VGA test writes a known pixel pattern to `/dev/fb0` and compares a
  QEMU `screendump` against a golden PPM.
- F1-F6 still switch to existing text virtual consoles; F8 switches to the
  GUI and back without losing either mode.
- `/dev/ptmx` can create a PTY, `/dev/pts/N` behaves as a tty, and `isatty()`
  plus `TIOCSWINSZ` work for a shell running inside a terminal window.
- A desktop process draws a wallpaper, panel, cursor, and overlapping windows.
- A terminal window runs `/bin/oksh`, supports resize, scrolling, SGR colors,
  and job-control signals.
- An image viewer opens, zooms, and pans BMP/PNG images.
- A file manager lists directories on the ramdisk and mounted ext2/vfat
  filesystems and can open, copy, rename, and delete files.
- Desktop labels, window titles, the terminal, image viewer, and file manager
  render text from a bundled TrueType font instead of the embedded bitmap font.
- Booting without VBE information still falls back to the existing 80x25 text
  console.

## Non-Goals

- No 2D/3D GPU acceleration, display hotplug, multi-monitor, or EDID/DDC in
  the first implementation.
- No X11/Wayland compatibility, dynamic linking, or DBus.
- No network-backed applications; the first desktop is local only.
- No complete text shaping engine such as HarfBuzz. TTF rendering initially
  covers Latin, box-drawing, and common symbols; complex scripts are deferred.
- No persistent desktop settings database. Initial configuration is compiled
  in or read from simple text files.

## Decisions, Constraints, and Exit Gates

This plan has two intentionally separate deliverables. They must not be
combined into one untestable graphics rewrite:

1. **Framebuffer platform:** boot reliably in a known VBE mode, preserve the
   text consoles, expose a safe framebuffer/input ABI, and prove pixels with
   automated screenshots.
2. **Desktop platform:** use that ABI to run a compositor, terminal, and
   applications. A desktop feature never changes the framebuffer ABI without
   a versioned ioctl or protocol change.

The following decisions are fixed for the first implementation:

- The primary automated-QEMU mode is **1024x768x32 XRGB**. It fits comfortably
  in the current 256 MiB QEMU configuration (one scanout plus two full-size
  32-bit compositor buffers use about 9 MiB), is easy to screenshot, and is
  widely supported by QEMU's standard VGA device. 1920x1080x32 remains an
  interactive target once the mode-selection path is proven; it is not a
  requirement for the first green test.
- The bootloader selects the VBE mode. The protected-mode kernel only consumes
  the Multiboot framebuffer record; it never executes VBE BIOS calls or walks
  a real-mode VBE mode list.
- `/dev/fb0` is a single-scanout device. It has no kernel compositing,
  page-flipping, or mode-setting in the first release. All composition occurs
  in userspace into one back buffer, followed by a damaged-rectangle copy to
  the scanout.
- A single `mario-desktop` process owns `/dev/fb0` and raw input in the first GUI
  milestone. The window manager, panel, terminal, and demonstration apps may
  initially be modules in that process. Unix-domain GUI clients are a later,
  separately testable phase.
- Text VTs remain authoritative for kernel diagnostics and recovery. GUI mode
  is optional. Any graphics initialization, allocation, or userspace-launch
  failure returns to a usable text VT and serial console.

No phase may start until its entry conditions are met, and it is complete only
when its listed test gate passes. In particular, do not start PTYs, FreeType,
or a multi-process protocol before the framebuffer platform has a stable,
screenshot-tested ABI.

### Compatibility Matrix

| Boot/display environment | Required behavior |
| --- | --- |
| QEMU `-vga std`, Multiboot framebuffer available | Start in requested 1024x768x32 when supplied; otherwise accept the loader-selected supported direct-color mode. |
| QEMU `-vga std`, valid framebuffer but non-32-bpp | Expose the true format through `/dev/fb0`; compositor converts from internal XRGB8888. |
| Custom `boot.asm` path without VBE handoff | Keep the current VGA text console; do not create `/dev/fb0`. |
| Invalid Multiboot framebuffer fields, unsupported memory model, or impossible mapping | Log one concise reason to serial/text console and continue in text mode. |
| GUI process exits, crashes, or cannot allocate its back buffer | Restore the prior text VT, release raw input, and leave `/dev/fb0` available for a later restart. |

### Required Ownership and Concurrency Rules

- Kernel framebuffer metadata is immutable after successful early
  initialization. It is copied from Multiboot memory before that memory can be
  reclaimed.
- `/dev/fb0` permits multiple read-only opens for diagnostics but exactly one
  writer or writable-mmap owner. `O_WRONLY`, `O_RDWR`, and writable mappings
  fail with `-EBUSY` while the compositor owns the device. A later
  capability/seat model may relax this rule; it is not part of phase 0.
- The compositor is the only process that writes scanout pixels in GUI mode.
  Clients never receive a direct `/dev/fb0` descriptor.
- Raw input is routed to exactly one foreground consumer: the selected text
  VT or `mario-desktop`. The kernel retains F1-F6/F8 handling before routing the
  remaining key event, so a wedged desktop cannot trap the user in graphics
  mode.
- No kernel framebuffer path may take a tty, task, or filesystem lock while
  holding the framebuffer metadata lock. Pixel copies run without that lock
  after validating the immutable range.

## Open Source Library Policy

Mario already vendors and statically links open source userspace libraries
such as zlib, ncurses, and Python. The GUI should follow the same pattern.

Allowed for the GUI:

- **FreeType** for TrueType/OpenType font loading, metrics, kerning, and glyph
  rasterization.
- **libpng** for PNG decoding.
- **libjpeg-turbo** for JPEG decoding.
- **zlib** for decompression used by PNG.
- **pixman** optionally for optimized software pixel compositing.
- **stb_truetype** only as a tiny fallback if the FreeType build is too large
  for the first ramdisk.

Constraints:

- Build every library with `util/mario-musl-cc` and link statically.
- Prefer MIT, BSD, zlib, or similarly permissive licenses. FreeType is
  dual-licensed under the FTL and GPL; use the FTL terms.
- Keep library sources under `app/usr/` and add `build-<lib>.sh` plus
  `<lib>.mk` files consistent with the existing `app/usr/Makefile`.
- Do not introduce dynamic linking or host shared-library assumptions.
- Record exact source version and license in the corresponding `.mk` header
  comments, as existing app libraries do.

Build and size budget:

- Target no more than roughly 2 MiB of additional stripped, statically linked
  GUI support in the first ramdisk: about 600 KiB for FreeType, 250 KiB for
  libpng plus zlib, 450 KiB for libjpeg-turbo, and 700 KiB for `libmario-ui`
  and the initial desktop applications combined. These are planning ceilings,
  not correctness limits; if they are exceeded, trim FreeType modules and
  switch to `stb_truetype` before reducing features.
- Keep all generated binaries static and `strip` them in the image build.
- Measure the ramdisk delta in `make image` and keep the GUI optional behind a
  Makefile flag until the size is stable.
- Add `GUI ?= false` to the root Makefile. When `GUI=true`, `make app` builds
  the GUI libraries/apps and `rd/make-rd0.sh` copies their outputs into the
  ramdisk. When false, the existing text-only build is unchanged.
- Target `_LIBC=musl` first because the GUI libraries and FreeType/libpng/
  libjpeg-turbo builds are validated with `util/mario-musl-cc`. Custom-libc
  builds are a follow-up, not a step-0 requirement.

Version and license baseline:

- Pin versions in the `.mk` files at adoption time. Use the table below as the
  expected baseline, updating it when a different version is actually vendored.
- FreeType: expected baseline 2.13.x; use FTL terms.
- libpng: expected baseline 1.6.x; libpng license.
- libjpeg-turbo: expected baseline 3.x; BSD-style license.
- zlib: reuse the version already vendored by Mario; zlib license.
- LVGL if adopted: expected baseline 9.x; MIT license.

Fallback policy:

- If FreeType cannot be built for musl/i386, use `stb_truetype` for the first
  milestone but keep the `gui_font` API unchanged.
- If libpng or libjpeg-turbo cannot be built, make the image viewer degrade
  gracefully: BMP always works; PNG/JPEG show an explicit unsupported-format
  message rather than failing to launch.
- If LVGL does not fit, use the custom `gui_*` widgets only. LVGL is an
  accelerator, not a hard dependency.

## Desktop and Window Manager Alternatives

Suitable open source alternatives exist, but none drops cleanly onto Mario
because Mario has no X11/Wayland, no Unix sockets yet, and no dynamic linking.

Best fits:

- **LVGL** is the best fit for widgets. It is MIT-licensed, runs directly on a
  framebuffer, already supports custom display/input drivers, and can be used
  as the widget toolkit underneath Mario's window manager. It does not provide
  a traditional overlapping-window manager by itself.
- **Microwindows/Nano-X** is the closest complete open source window system.
  It has a small framebuffer server, Nano-X client API, basic window manager,
  and widget set. It is a larger adoption commitment and depends on Unix
  sockets/shared memory or its own transport.
- **tinywm** is a useful reference for window-management semantics, but it is
  an X11 client and would require an X server; use it only for API/behaviour
  inspiration.
- **SDL** can simplify framebuffer, input, and image handling, but it is a
  media library, not a desktop or window manager.

Recommended decision:

- Use **LVGL** for widgets and rendering primitives where possible.
- Keep the Mario window manager/compositor small and custom because it must
  own the framebuffer, route `/dev/tty7` and `/dev/psaux` events, and integrate
  with Mario's VT switching and PTY model.
- Treat Microwindows as the fallback architecture reference, not the initial
  implementation.

## Architecture

```text
Userspace
  +-----------------------------------------------------------+
  | Apps: terminal, image viewer, file manager, app launcher  |
  +-----------------------------------------------------------+
  | Desktop shell / window manager / compositor                |
  +-----------------------------------------------------------+
  | libmario-ui: surfaces, TTF fonts, widgets, events, images  |
  +-----------------------------------------------------------+
  | libc: open/read/write/ioctl/poll/fork/exec                 |
  +-----------------------------------------------------------+
Kernel
  +-----------------------------------------------------------+
  | /dev/fb0 | /dev/ttyN | /dev/psaux | /dev/ptmx, /dev/pts/N  |
  +-----------------------------------------------------------+
  | console switch + framebuffer map + input IRQ + tty core    |
  +-----------------------------------------------------------+
```

The first GUI milestone can run the desktop and applications in one process to
avoid introducing inter-process GUI IPC before the kernel has Unix sockets.
The final target is multi-process: the window manager is the GUI server and
each application is a client. A minimal `AF_UNIX` stream-socket transport is
the planned IPC mechanism because it is a natural fit for event messages and
can later pass file descriptors with `SCM_RIGHTS` if needed.

## Kernel Foundations

### 1. Request a Multiboot Video Mode

**Ownership boundary:** the Multiboot producer (GRUB/QEMU loader or the
future real-mode custom bootloader) chooses and sets a VBE mode. The kernel
does not enumerate VBE modes: BIOS interfaces are unavailable and inappropriate
after entering protected mode. The kernel validates the resulting Multiboot
framebuffer record and either maps it or stays in text mode.

In `src/include/multiboot.h`:

- Add `MB_FLAG_VIDEO` as an alias for the existing `MB_FLAG_VBE`.
- Add Multiboot video mode type constants:

  ```c
  #define MB_VIDEO_TYPE_GRAPHICS 0
  #define MB_VIDEO_TYPE_TEXT     1
  ```

- Add a packed VBE Mode Info Block structure with the fields the kernel
  consumes:

  ```c
  uint16_t mode_attributes;
  uint16_t bytes_per_scanline;
  uint16_t x_resolution;
  uint16_t y_resolution;
  uint8_t  bits_per_pixel;
  uint8_t  memory_model;
  uint32_t phys_base_ptr;
  ```

In `src/kernel/start.S`:

- Change `MB_FLAGS` to `0x00010403` (existing flags plus `MB_FLAG_VBE`:
  `0x00010003 | 0x00000400`). Keep `MB_CHECK` as `-(MB_MAGIC + MB_FLAGS)` so
  the header checksum updates automatically.
- Append the preferred-mode words after the existing address fields:
  mode type `0`, width `1024`, height `768`, depth `32`. A later interactive
  build setting may request 1920x1080 once the baseline screenshot test is
  stable.

The preferred mode is a request. For "always use the highest supported mode",
the *custom bootloader* must enumerate VBE modes before protected mode:

1. Call VBE Function `00h` and verify the `"VESA"` signature.
2. Read the protected-mode `VideoModePtr`.
3. Walk the mode list until `0xFFFF`.
4. Query each mode with VBE Function `01h`.
5. Keep graphics modes with linear framebuffer or explicit banking, packed
   pixel/direct color, 15/16/24/32 bpp, and nonzero resolution/stride/base.
6. Sort by resolution, then depth, then linear-framebuffer preference.
7. Set the selected mode with VBE Function `02h`, using `0x4000` for linear
   mode.
8. Populate a standards-compliant Multiboot framebuffer/VBE handoff record
   before entering the kernel. Do not invent a private pointer into bootloader
   memory that the kernel cannot validate after paging changes.

Fallback selection rule:

- For the first QEMU test image, request `1024x768x32`. Once the raw
  framebuffer test passes, the interactive configuration may request
  `1920x1080x32`.
- If the requested mode is not available, select the highest-resolution
  graphics mode with a linear framebuffer. Tie-break in this order:
  resolution, then bpp (`32`, `24`, `16`), then bytes per scanline.
- The kernel and `/dev/fb0` must always report the mode that was actually
  selected. No VGA golden image may hardcode `1920x1080`; tests read
  `FBIOGET_VSCREENINFO` and normalize the expected image accordingly.
- For a non-32-bpp fallback mode, `/dev/fb0` still exposes the real masks and
  offsets; `libmario-ui` performs the conversion on every flush.

For QEMU's built-in Multiboot loader, the preferred-mode header is sufficient
for the first test image. For the custom `src/boot/boot.asm`, VBE enumeration
is a later bootloader milestone. Until then, the custom-loader path retains
the existing text fallback. This is a deliberate compatibility split, not a
partial kernel implementation.

EDID/DDC is out of scope. This plan selects the highest VBE BIOS mode rather
than the monitor's native physical mode.

### 2. Capture and Map the Framebuffer

Add:

- `src/include/video/fb.h`
- `src/video/fb.c`

Core state:

```c
struct fb_info {
    int enabled;
    uint32_t phys_base;
    void *virt_base;
    uint32_t size;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    uint8_t  bpp;
    uint32_t red_mask, green_mask, blue_mask;
    uint8_t  red_pos, green_pos, blue_pos;
};
```

Lifecycle:

1. `fb_early_init(struct multiboot_info *m)` copies the VBE fields while low
   Multiboot memory is still mapped.
2. After `paging_init()`, `fb_map()` creates kernel page-table entries for the
   framebuffer physical range.
3. Use an explicit reserved kernel virtual window such as `0xF0000000`
   (`KERNEL_BASE + 0x70000000`). Validate it against RAM mappings.
4. If VBE data is absent or invalid, leave `fb_info.enabled == 0` and continue
   with text mode.

Validation and mapping rules:

- Accept only direct-color/packed-pixel framebuffer records with nonzero base,
  width, height, stride, and bpp. Reject text-mode, planar, banked-only, and
  palette-only records in phase 0.
- Compute `size = stride * height` with checked 64-bit arithmetic. Reject a
  size of zero, a size above the reserved framebuffer virtual-window capacity,
  or an address range that wraps `phys_base + size`.
- Page-align the physical start downward and the end upward. Keep both the
  original `phys_base` and the page-aligned map base so `/dev/fb0` offsets are
  relative to the first visible pixel, not the mapping padding.
- Map the range uncached or write-combining when the paging implementation can
  express that safely. Until then, use the existing i386 uncached device-map
  convention and document the slower full-copy behavior; never map the MMIO
  range as normal reclaimable RAM.
- Reserve a named `FB_VIRT_BASE`/`FB_VIRT_LIMIT` range in one header. No other
  `ioremap`-style consumer may allocate from it ad hoc.
- `fb_early_init()` must be idempotent. The init path logs the selected mode
  once and never attempts a late fallback mode switch.

The early-copy/late-map split is required because `paging_init()` currently
unmaps the low 16 MiB after `ramdisk_setup()`.

### 3. Raw Framebuffer Device `/dev/fb0`

Add:

- `src/fs/char/fb.c`
- `FB_MAJOR 13`/`FB_MINOR 0` in `src/include/fs/chrdev.h` (major 13 is below
  the current `MAX_CHRDEV` limit and not already claimed by mem/tty devices)
- `fb0 C 13 0` in `rd/device_list`

Initial operations:

- `open`: select `/dev/fb0`, initialize file position to zero.
- `lseek`: permit positions in `0..fb.size`.
- `read`: copy from `fb.virt_base + pos` into the user buffer.
- `write`: copy from the user buffer into the framebuffer.
- `release`: no-op.
- `ioctl`: implement `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, and return
  `-EINVAL` for `FBIOPUT_VSCREENINFO` until mode switching is supported.
- `mmap`: phase 0 may defer this until physical-device VMA support exists;
  phase 1 adds a bounded `MAP_SHARED` mapping only for the exclusive writer.

Use `verify_area()` and `memcpy_tofs()`/`memcpy_fromfs()` for all user copies.

File and mmap semantics:

- `read` and `write` clamp at `fb.size`; they return a short count at the end
  and never wrap file position. Negative seeks and seeks past `fb.size` fail
  with `-EINVAL`.
- Writable `mmap` validates `vm_pgoff`, page-aligns its requested range, and
  rejects mappings extending beyond the page-rounded visible framebuffer
  allocation. It maps framebuffer PFNs directly; it does not use anonymous
  pages, copy-on-write, or the regular file page cache.
- `MAP_PRIVATE`, executable framebuffer mappings, and `FBIOPUT_*` are rejected
  in phase 1. There is no supported mode switch from an open framebuffer fd.
- On last exclusive-writer close, the kernel does not clear the scanout. It
  returns input routing to the prior text VT and leaves the final frame visible
  until the next owner opens the device or the user switches VTs.

Framebuffer ioctl ABI:

- Define Mario's own small `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`
  structures in `src/include/video/fb.h`; do not require Linux's exact struct
  layout.
- `struct fb_var_screeninfo` fields: `xres`, `yres`, `xres_virtual`,
  `yres_virtual`, `bits_per_pixel`, `red_offset`, `red_length`,
  `green_offset`, `green_length`, `blue_offset`, `blue_length`.
- `struct fb_fix_screeninfo` fields: `id[16]`, `smem_start`, `smem_len`,
  `type`, `visual`, `line_length`.
- `FBIOPUT_VSCREENINFO` returns `-EINVAL` in step 0. Add a separate
  `FBIOGET_CONSOLE_STATE` value in step 1.
- Keep these ioctl numbers stable in the header; changing them after a
  userspace build would break the GUI binaries.

Framebuffer format contract:

- 32 bpp modes are normalized as `XRGB8888` (red in bits 16-23, green in
  bits 8-15, blue in bits 0-7, alpha ignored by the hardware).
- 24 bpp modes are packed RGB888 with three bytes per pixel.
- 16 bpp modes are supported only as a fallback; userspace treats them as
  RGB565 unless the VBE mode data says otherwise.
- `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO` report the real masks, offsets,
  line length, and memory size. `libmario-ui` keeps internal surfaces in
  `XRGB8888` and converts only when flushing to `/dev/fb0`.

Device ownership and access:

- In Phase 1 the GUI process is the only process that opens `/dev/fb0`; normal
  applications never draw directly to it.
- In Phase 2 the window-manager server keeps `/dev/fb0` open and never passes
  the fd to clients. Client surfaces are composited by the server.
- Create `/dev/fb0` with root-only permissions (0600 initially). If Mario
  gains a permission system later, add a dedicated `video` group.
- On switch-away from the graphics console, writes to `/dev/fb0` are either
  rejected with `-EBUSY` or queued and marked dirty for redraw on switch-back.
  The initial implementation rejects them to make ownership mistakes obvious.
- Reads are allowed for debugging and screen captures, but they do not grant
  ownership or bypass the switch state.

### 4. Graphics Console and VT Switching

Keep the existing text console emulator as the fallback. Refactor cell drawing
only if a kernel-rendered high-resolution text console is desired later:

```c
console_paint_cell(struct console *con, int row, int col, uint16_t cell);
console_paint_rect(struct console *con, int row, int col, int rows, int cols);
console_paint_cursor(struct console *con);
```

Console layout:

- `/dev/tty1` through `/dev/tty6`: existing text consoles.
- `/dev/tty7`: graphics console owned by the GUI process.
- serial: existing serial consoles.

Add:

```c
enum console_type {
    CONSOLE_TYPE_TEXT,
    CONSOLE_TYPE_GRAPHICS,
};
```

`switch_fg_console()` behavior:

- F1-F6 selects a text console and redraws its cell buffer.
- F8 selects the graphics console. The kernel does not draw text into it.
- Switching away from F8 notifies the GUI process that it no longer owns the
  framebuffer; switching back notifies it to redraw.
- Add `FBIOGET_CONSOLE_STATE` to `/dev/fb0`. It returns `0` while a text
  console is foreground and `1` while the graphics console is foreground.
- `poll()` on `/dev/fb0` wakes with `POLLPRI` when the state changes. The GUI
  event loop treats `POLLPRI` as a console-change event, calls
  `FBIOGET_CONSOLE_STATE`, and does a full desktop redraw when state returns
  to `1`.
- `/dev/tty7` is opened `O_RDWR | O_NONBLOCK` and configured with the existing
  tty raw-mode ioctls (`cfmakeraw`-equivalent `termios` settings). F8 remains
  a kernel VT-switch key and is not delivered as ordinary input.

Model the ownership transition on Linux `KD_GRAPHICS`/`KD_TEXT`.

### 5. Keyboard Input

The existing `src/fs/char/kbd.c` already decodes PS/2 keyboard bytes for text
consoles. The GUI needs two related improvements:

1. A raw console mode that exposes bytes or key events without line discipline
   and terminal echo processing. `/dev/tty7` in raw mode is sufficient for the
   first implementation.
2. A stable keysym layer. Convert scancodes to a small `keysym` set used by
   `libmario-ui`: printable characters, arrows, Home/End, Page Up/Down,
   Insert/Delete, F1-F12, Shift/Ctrl/Alt modifiers, and media/function keys
   used for desktop shortcuts.

The kernel should remain small: keyboard IRQ feeds the existing tty input ring.
The GUI can consume `/dev/tty7` bytes and perform keysym translation in
userspace initially. A future `/dev/input/event0` interface is optional.

Keyboard translation details:

- Userspace maintains a small scancode table with modifier state. Auto-repeat
  starts after a 500 ms delay and then repeats at 30 Hz while the key is held;
  the repeat policy lives in `libmario-ui`, not the kernel.
- The initial keysym set is enough for ASCII input plus arrows, Home/End, Page
  Up/Down, Insert/Delete, F1-F12, Shift, Ctrl, Alt. Caps Lock toggles an
  internal modifier flag.
- F8 is consumed by the VT switcher and never appears as a `gui` key event.
- Unmapped scancodes are ignored but logged in a bounded debug ring; they must
  not wedge the input stream.

### 6. Mouse Input

Mouse support is required for a useful window manager and file manager. The
current `ps2_init()` disables the PS/2 auxiliary port; this must be changed.

Kernel work:

- Enable the PS/2 controller auxiliary device and IRQ 12.
- Decode the standard 3-byte PS/2 mouse packet:
  - left/middle/right buttons;
  - signed X/Y deltas;
  - optional wheel packet for IntelliMouse.
- Expose relative movement through a character device, initially
  `/dev/psaux` or `/dev/input/mouse0`.
- Make the device `read()`/`poll()` compatible so the GUI event loop can wait
  on both keyboard and mouse fds.

Userspace work:

- Accumulate relative deltas into an absolute pointer position clipped to the
  framebuffer rectangle.
- Convert button/wheel transitions into press/release/wheel events.
- Render a cursor sprite as the topmost compositor layer.

The initial implementation only supports one pointer. Multi-touch is out of
scope.

`/dev/psaux` read format:

```c
struct psaux_event {
    int16_t dx;
    int16_t dy;
    int16_t dz;      /* 0 for a basic mouse, +/-1 wheel otherwise */
    uint8_t buttons; /* bit 0 left, bit 1 right, bit 2 middle */
};
```

- Each `read()` returns one complete event or `EAGAIN` when `O_NONBLOCK` and
  no event is pending. Partial reads never occur.
- `poll()` reports `POLLIN` when at least one event is available and `POLLHUP`
  if the auxiliary device is absent or fails.
- The driver drops packets on overflow instead of blocking the IRQ path; a
  userspace polling loop can safely miss intermediate motion but must never
  observe a half-packet.

### 7. Pseudo-Terminals

The standalone PTY and tmux work is specified in
`doc/PTY_TMUX_PLAN.md`. That document owns the Unix98 ABI, tty/session
semantics, AF_UNIX prerequisite, package builds, and acceptance tests below.
This section records only how that independently shippable work is consumed by
the later GUI terminal milestone.

The GUI terminal must use a real tty. The earlier pipe-based shell was a
prototype only; job control, `isatty()`, `TIOCSWINSZ`, and terminal signal
delivery need a PTY.

Choose the Unix98 PTY model because it matches musl:

- `/dev/ptmx` is the cloning master.
- `/dev/pts/N` are slave devices.
- The master and slave are paired file objects.
- Use static `/dev/ptmx` and `/dev/pts/0..31` device nodes in `rd/device_list`
  rather than introducing a separate `devpts` filesystem or mount point in
  the first implementation. This keeps the device namespace consistent with
  Mario's existing static `/dev` layout.

Kernel changes:

1. Split the existing tty line discipline and ring-buffer code from
   `src/fs/char/tty.c` into a reusable tty core.
2. Add `src/fs/char/pty.c` and register the `devpts`-style inode namespace.
3. Implement master file operations:
   - `read`/`write`;
   - `poll` for readability/writability;
   - `ioctl` for `TIOCGPTN`, `TIOCSPTLCK`, `TIOCSWINSZ`.
4. Implement slave file operations:
   - reuse console/tty line discipline;
   - support `termios`, `TIOCGWINSZ`/`TIOCSWINSZ`, and `TIOCGPGRP`/
     `TIOCSPGRP` for job control.
5. Connect slave `SIGINT`/`SIGQUIT`/`SIGTSTP` generation to the foreground
   process group stored in the tty.
6. Clean up the master/slave pair when both ends are closed.

PTY lifecycle and limits:

- Start with 32 PTY pairs (`/dev/pts/0` through `/dev/pts/31`), which is
  enough for the desktop, a few terminals, and the initial apps. Make the
  limit a `#define` in `pty.c`; do not build dynamic allocation until it is
  measured as necessary.
- A pair becomes free only after both the master and slave are closed and all
  child file references are released. Reuse the lowest-numbered free slot so
  stale `/dev/pts/N` names are recycled deterministically.
- On the final close of either end, hangup the paired side, wake pollers with
  `POLLHUP`, and signal the foreground process group with `SIGHUP` before
  freeing the pair.
- `TIOCSPTLCK` prevents the corresponding `/dev/pts/N` from being opened by
  any process; the terminal app calls it after opening the slave. Unlocking is
  not required for the first milestone.
- Keep the PTY input/output buffers bounded. Use 4096-byte ring buffers per
  direction initially and make `write()` apply tty flow control rather than
  growing without limit.
- Record the window size as width/height rows and columns in the tty struct.
  `TIOCSWINSZ` on either master or slave updates the same shared size and wakes
  the other end where relevant.

Test cases:

- Open `/dev/ptmx`, verify `TIOCGPTN` returns a slave number.
- Open `/dev/pts/N` and confirm `isatty(STDIN_FILENO)` after `dup2`.
- Write `printf 'abc\n' | cat` inside the PTY and read `abc` from the master.
- Resize the terminal window and verify `TIOCSWINSZ` reaches the shell.
- Run `yes | head`, stop it with Ctrl-C, and verify the foreground process
  group receives `SIGINT`.

### 8. GUI Inter-Process Communication

The eventual window manager/client split needs a small IPC primitive. The
recommended path is a minimal `AF_UNIX` `SOCK_STREAM` implementation:

- `socket`, `bind`, `listen`, `accept`, `connect`, `send`, `recv`, `poll`,
  `close`, and `socketpair`.
- Stream semantics only for the first version; datagrams can be added later.
- `sendmsg`/`recvmsg` are useful but not required initially. Applications can
  inherit their PTY slave or framebuffer-related file descriptors at fork/exec
  time.

If Unix sockets are deferred, the desktop can still be shipped as one process
that hosts all applications as pthreads. The GUI protocol should be defined as
messages over a byte stream so the transition to sockets is mechanical.

GUI protocol message families:

- `WINDOW_CREATE`/`WINDOW_DESTROY`;
- `WINDOW_MOVE`/`WINDOW_RESIZE`;
- `WINDOW_SET_TITLE`;
- `SURFACE_UPDATE` or dirty rectangle notifications;
- `INPUT_EVENT`;
- `WINDOW_FOCUS`/`WINDOW_RAISE`;
- `APP_READY`/`APP_EXIT`.

The server owns the framebuffer and input devices. Clients never write to
`/dev/fb0` directly in the multi-process model.

Socket and message details:

- Phase 2 server listens on `/tmp/.mario-wm/socket` with mode `0600`. The
  server removes any stale socket path at startup before binding.
- Backlog is fixed at 8. Reject additional connects with `ECONNREFUSED` or a
  busy status until a client disconnects.
- The socket namespace is ordinary filesystem pathnames only; no Linux
  abstract-socket namespace because Mario's pathname lookup does not support it.
- Messages are length-prefixed with a 32-bit little-endian byte count. Maximum
  message size is 64 KiB; larger payloads are rejected with `EMSGSIZE`.
- `SURFACE_UPDATE` carries a dirty-rectangle list and compressed/uncompressed
  32-bit pixel rows. Phase 2 starts with uncompressed rows; simple RLE can be
  added later only if profiling shows socket copy dominates.
- On disconnect, the server releases all window surfaces, input grabs, and
  file descriptors associated with that client.

## Userspace GUI Platform: `libmario-ui`

Create a static userspace library under:

```text
app/usr/libgui/
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

### Core Types

```c
typedef struct { int x, y, w, h; } gui_rect;
typedef uint32_t gui_color;

typedef struct {
    int width;
    int height;
    int stride;
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
    GUI_EVENT_MOUSE_WHEEL,
    GUI_EVENT_WINDOW_CLOSE,
    GUI_EVENT_WINDOW_RESIZE,
    GUI_EVENT_TIMER
} gui_event_type;

typedef struct {
    uint64_t timestamp_ms;
    gui_event_type type;
    uint32_t window_id;
    union {
        struct { uint32_t keysym; uint32_t modifiers; } key;
        struct { int x, y; uint8_t buttons; int dz; } mouse;
        struct { int width, height; } resize;
    };
} gui_event;
```

### Drawing Primitives

Initial implementation:

- fill rectangle with solid color;
- copy/blit one surface into another with clipping;
- alpha blend one surface over another;
- draw horizontal/vertical lines;
- draw 1-pixel rounded rectangles for window chrome;
- draw text using the TTF glyph cache with an embedded 8x16 CP437 fallback;
- set a damage rectangle after every draw operation.

The framebuffer format is `XRGB8888` for 32 bpp. If fallback mode is 24 or 16
bpp, `libmario-ui` keeps internal surfaces in 32 bpp and converts on flush.

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
- Damage regions are merged before compositing. Overlapping rectangles may be
  coalesced if their union does not exceed twice the sum of their areas.
- The compositor never reads damage regions from an app-owned surface after
  the app has modified it without taking the window lock; in Phase 2 the
  damage list travels with `SURFACE_UPDATE`, so there is no shared-memory race.
- Alpha blending operates in premultiplied `XRGB8888` internally, then stores
  the non-premultiplied RGB result expected by the framebuffer.

Pixel conversion:

- `gui_surface` is always 32-bit. The flush path converts `XRGB8888` to the
  `/dev/fb0` format with a small per-bpp conversion table or inline function.
- For RGB565, use 5/6/5 truncation with round-to-nearest. For packed RGB888,
  write bytes in memory order rather than assuming endianness.
- The `FBIOGET_*` mask values are used to build the conversion table once at
  startup; no hardcoded host-endian assumptions remain in the userspace code.

Surface memory and failure handling:

- `gui_surface_create()` returns `NULL` on allocation failure. Callers must
  show an in-band error status or close the window; they must never dereference
  a failed surface.
- The window manager limits total live window pixels to 2.5 times the
  framebuffer size initially. Applications requesting a larger surface get
  the requested geometry but a tiled/partial backing store is deferred, so the
  request is clamped.
- Surfaces are explicitly freed on window close and on client disconnect.
  There is no garbage collector or reference-counted surface API in Phase 1.
- Temporary scratch buffers used by image decode and glyph rasterization are
  capped: image decode uses one 4 MiB scratch buffer, and font glyph cache
  uses an LRU limit of 2048 entries.

### TrueType Font Rendering

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

- Bundle an open-source TrueType font, initially a monospace face such as
  DejaVu Sans Mono or Liberation Mono, under `rd/usr/share/fonts/`.
- Provide a proportional font for desktop labels and a monospace font for the
  terminal and file manager columns.
- `gui_font_load(path)` calls `FT_New_Memory_Face()` on the file contents and
  selects a point size.

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

### Window Surfaces

Each window has a backing `gui_surface`. Drawing to a window never touches the
framebuffer directly; the compositor copies dirty rectangles to `/dev/fb0`.

Window properties:

- position and z-order;
- title string;
- minimum and maximum size;
- focus state;
- dirty rectangle list;
- optional icon/pixmap.

### Widgets

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

### Event Loop

The desktop uses `poll()` on:

- `/dev/tty7` for keyboard bytes;
- `/dev/psaux` for mouse packets;
- timer fd/pipe for periodic ticks;
- application socket fds in the multi-process model.

Events are timestamped, queued, and dispatched to the focused window or the
window manager.

Event ordering and concurrency:

- Input is processed in the order returned by `poll()` and carries a monotonic
  timestamp at read time. Timestamp ties are broken by fd order: keyboard,
  mouse, timer, then GUI socket.
- Phase 1 uses no app threads for the initial shell; the event loop, layout,
  and compositing run on one thread. Long-running work such as image decode is
  explicitly cooperative or moved to a worker thread that only publishes a
  finished `gui_surface` through a locked completion queue.
- If Phase 1 hosts apps as pthreads, protect the window list, z-order, focus,
  and damage regions with one `gui_wm_lock` mutex. The compositor copies damage
  lists while holding that lock, then releases it before writing to `/dev/fb0`.
- Mouse events are delivered to the window under the pointer unless a button
  press starts a move/resize or popup grab. During a grab, all mouse events go
  to the grab owner until release; keyboard focus is unchanged.
- A window receives keyboard events only when it has focus. The window manager
  intercepts global shortcuts before routing to the focused window.
- In Phase 2, socket messages are length-prefixed and enqueued as events. The
  server processes each message fully before polling again, which keeps client
  operations atomic without locks between processes.

### Image Decoding

Use small internal code for BMP and mature open source libraries for the
compressed formats:

- BMP: uncompressed 24/32 bpp, bottom-up or top-down;
- PNG: 8-bit RGB/RGBA through libpng and zlib;
- JPEG: baseline grayscale/RGB through libjpeg-turbo.

Decoded images are converted to `gui_surface` and cached by the image viewer.
Large images may be downscaled to fit available heap.

Vendor `libpng` under `app/usr/libpng/` and `libjpeg-turbo` under
`app/usr/libjpeg-turbo/`. Add `build-libpng.sh`/`libpng.mk` and
`build-libjpeg-turbo.sh`/`libjpeg-turbo.mk` matching the existing app build
style. The image viewer links these libraries statically.

## Desktop and Window Manager

### Process Model

Phase 1:

- `mario-desktop` is a single process.
- It owns the framebuffer and input devices.
- It creates windows directly and hosts the terminal, image viewer, and file
  manager as internal views or pthreads.

Phase 2:

- `mario-wm` is the GUI server and compositor.
- Applications are separate processes connected by `AF_UNIX` sockets.
- `mario-desktop` is the shell/panel/launcher client.

This document uses the Phase 2 layout as the target, with Phase 1 as the first
shippable integration point.

### Window Manager Responsibilities

- Maintain z-order and focused-window state.
- Own all framebuffer drawing after the GUI console becomes active.
- Route keyboard and mouse events.
- Provide window move, resize, raise, close, and title-bar hit testing.
- Clip dirty rectangles to the visible region of each window.
- Render window chrome and the mouse cursor.
- Send close/resize events to applications.
- Redraw the whole desktop on GUI-console activation.

Focus, raising, and input routing:

- Use click-to-focus: a mouse-down in a window raises it and makes it focused;
  a click on desktop lowers focus to the desktop and clears the focused
  window. Keyboard focus changes only on this explicit interaction.
- Z-order is a single top-to-bottom list. Raising moves a window to the top
  without changing its geometry. The desktop is always the bottom layer.
- Title-bar dragging moves a window; border/corner dragging resizes it. The
  resize is clipped to the minimum size and the screen bounds.
- If a window is moved/resized by the application, the WM treats that as a
  request, clamps it, and sends back the actual geometry in the event or
  protocol reply.
- The focused window is drawn with a highlighted title bar; unfocused windows
  are drawn with an inactive title color. Focus highlight never changes
  application content, only chrome.
- Alt-Tab cycles through the most-recently-used order, not z-order. Alt-F4
  sends `WINDOW_CLOSE` to the focused window.
- Input not claimed by a popup/modal grab goes to the window under the
  pointer. Modal dialogs are deferred to Phase 2; Phase 1 has no modal
  windows.

### Desktop Shell

The desktop client provides:

- wallpaper or solid background;
- top or bottom panel with app launcher, window list, and clock;
- taskbar buttons for open windows;
- basic keyboard shortcuts such as Alt-Tab, Alt-F4, and Super-Return for a
  terminal;
- optional desktop icons for terminal, file manager, image viewer, and
  shutdown/reboot.

### Compositing Strategy

The first compositor is simple:

1. Collect dirty rectangles.
2. Clip each dirty rectangle to each visible window from back to front.
3. Blit only the visible portions.
4. Blit the cursor sprite last.
5. Flush through `pwrite()` to `/dev/fb0`.

Full-screen flushes are acceptable for milestone one. Dirty-region and partial
flush optimization follow once window movement is stable. Vsync is not
required; use a fixed timer to throttle redraws.

## Terminal Window and PTY Integration

Add:

- `app/usr/terminal/main.c`
- `app/usr/terminal/term.c`
- `app/usr/terminal/vt.c`

The terminal app:

1. Opens `/dev/ptmx`.
2. Calls `grantpt`/`unlockpt` equivalents via `TIOCSPTLCK` and `TIOCGPTN`.
3. Opens `/dev/pts/N`.
4. Forks a shell child. The child calls `setsid`, opens the slave as its
   controlling tty, duplicates it to stdin/stdout/stderr, and execs
   `/bin/oksh`.
5. The GUI process keeps the master fd in its `poll()` set.
6. Master bytes are parsed by the VT emulator and written to the terminal
   window surface.
7. Key events are translated into escape sequences and written to the master.
8. Window resizes send `TIOCSWINSZ` to the slave and redraw the terminal grid.

### VT Emulator Scope

First version supports:

- cursor movement (`CUU`, `CUD`, `CUF`, `CUB`, `CUP`, `CHA`);
- erase operations (`ED`, `EL`);
- scroll regions (`CSR`) and screen scrolling;
- SGR colors, bold, inverse, and underline;
- save/restore cursor (`DECSC`/`DECRC`);
- alternate screen buffer;
- basic UTF-8 display for printable characters;
- glyph rendering through the TTF cache and fallback font;
- tab stops;
- `OSC` title updates.

Do not implement full xterm compatibility. The goal is enough ANSI/VT100
support for `oksh`, `vim`, `top`, `less`, and interactive Python.

Terminal behavior details:

- Keep at least 4096 lines of scrollback per terminal window. Normal screen
  scrolling appends old lines to a ring buffer; alternate-screen scrolling does
  not add to scrollback.
- Implement tab stops every eight columns, clear-tab, and set-tab behavior.
- Text selection and clipboard are deferred. The first terminal has no mouse
  selection or copy/paste because it requires a system clipboard service and
  more complete OSC 52 support. Keep `src/term.c` data structures ready for
  selection rectangles but do not ship the feature in the initial milestone.
- Mouse reporting modes (`1000`, `1002`, `1003`, `1006`) are deferred; apps
  that request them must still run without input breakage, so the emulator
  should ignore or store the mode rather than corrupt the terminal grid.
- Resize behavior: the terminal computes the largest grid that fits the window
  minus scrollbar/chrome, then calls `TIOCSWINSZ` with row/column counts. The
  grid is reflowed only for full-screen width changes; soft reflow is deferred.
- Set the shell environment before exec: `TERM=xterm-256color`, `COLUMNS` and
  `LINES` equal to the initial grid size, `HOME=/root`, and `SHELL=/bin/oksh`.
  Update `COLUMNS`/`LINES` through `TIOCSWINSZ` on resize; `oksh` reads the
  tty size for `stty size`.
- Child termination: when the shell exits, the terminal prints a status line
  such as `[process exited]`, keeps the scrollback visible, and closes on
  explicit window close or Enter.

## Applications

### Image Viewer

Add `app/usr/imageviewer/`.

Features:

- open an image from a path or from a file-manager launch;
- decode BMP and PNG first, then JPEG;
- fit-to-window, 1:1, and zoom modes;
- pan by mouse drag or keyboard arrows;
- next/previous file in the same directory;
- status line with image dimensions, file size, and zoom factor.

View state and image cache:

- `fit-to-window` preserves aspect ratio and centers the image. `1:1` maps one
  source pixel to one framebuffer pixel and allows panning when larger than
  the window. Zoom uses a geometric scale list (`0.25x`, `0.5x`, `1x`, `2x`,
  `4x`) initially rather than arbitrary fractional zoom.
- Pan is clipped to the intersection of the scaled image and the viewport; the
  cursor changes or a status hint indicates when panning is possible.
- Next/previous iterate the same directory in `readdir` order, skipping
  non-image files. The decoder is determined by extension first and by magic
  bytes second; a mismatch falls back to magic-byte detection.
- Cache at most two decoded frames: the current image and the next image
  preloaded in the background. Discard and re-decode older images. Decoded
  images larger than the framebuffer are downscaled with simple area
  averaging; no high-quality resampling is required.
- A decoder failure keeps the previous frame on screen and puts the error in
  the status line rather than closing the window.

Validation:

- A VGA test opens a known PNG/BMP, dumps the screen, and checks that the
  image is visible and the status line is correct.

### File Manager

Add `app/usr/filemanager/`.

Features:

- navigate `/`, `/root`, `/mnt`, and mounted ext2/vfat directories;
- display entries with name, type, size, and modification time;
- change directory, parent directory, and absolute path entry;
- open files with the matching application:
  - directories: navigate;
  - text files: terminal editor or `less`;
  - images: image viewer;
  - executables: execute in a terminal window;
- copy, move, rename, delete, and create directory;
- refresh after filesystem changes.

The file manager uses existing filesystem syscalls (`open`, `read`, `write`,
`mkdir`, `unlink`, `rename`, `stat`) and the GUI list widgets.

File-operation policy:

- Use `rename()` for move within the same filesystem. Copy across filesystems
  reads/writes blocks with a fixed 64 KiB buffer and preserves the mode bits
  that Mario supports.
- Before overwriting an existing destination, show a simple confirmation
  dialog in the GUI, or fail with `-EEXIST` in shell-mode tests. There is no
  trash/recycle bin in the first version.
- Directory delete is recursive only after an explicit confirmation; symlinks
  are deleted with `unlink()`, never followed during delete.
- Refresh the directory listing after every create, rename, delete, and
  successful copy. Refresh also after a child app exits and after manual F5.
- Text files open in the terminal app with `less` by default; image files open
  in the image viewer; executables run in a new terminal window with a shell
  command line quoted for `oksh`.
- When a launch target cannot be executed, keep the file manager open and show
  the failure in a status line/message rather than silently closing.

Validation:

- Shell/VGA tests exercise file-manager actions on both mariofs and an
  ext2/vfat mount. Do not use destructive operations on the host-mounted
  shared test images outside the test harness.

### App Launcher and System Panel

Add a simple launcher to the desktop panel:

- terminal;
- file manager;
- image viewer;
- about box;
- reboot/halt if the kernel exposes the existing reboot path to userspace.

The launcher is a grid of buttons above the panel.

Session lifecycle and configuration:

- Phase 1 starts `mario-desktop` from `/etc/init` or the existing startup
  script after `/dev/fb0` and input devices are available. The desktop process
  is the session leader for GUI apps.
- Phase 2 starts `mario-wm` first; `mario-wm` then spawns `mario-desktop`.
  App exit does not terminate the WM; WM exit or a killed server closes all
  client windows and returns to the text console.
- Keep initial settings in `/etc/mario-gui.conf` as `key=value` lines:
  wallpaper path or color, font paths, terminal command, panel position, and
  default image viewer/file manager paths. Missing keys use compiled-in
  defaults.
- Reboot/halt are present only if the kernel exposes a safe userspace call or
  device. If absent, the launcher omits those buttons instead of hardcoding a
  privileged path.
- The about box reports build date, Mario version, framebuffer mode, and
  library versions from the `.mk` metadata.

## Directory Layout

Kernel:

```text
src/include/video/fb.h
src/video/fb.c
src/fs/char/fb.c
src/fs/char/pty.c
src/fs/char/psaux.c
src/fs/char/tty.c          # refactor for shared line discipline
src/include/fs/tty/pty.h
src/include/fs/tty/psaux.h
src/include/fs/tty/tty.h
src/kernel/start.S
src/include/multiboot.h
src/mm/paging.c
```

Userspace:

```text
app/usr/libgui/
app/usr/desktop/
app/usr/terminal/
app/usr/imageviewer/
app/usr/filemanager/
app/usr/launcher/
app/usr/Makefile
app/usr/lvgl/              # optional widget implementation, if reused
app/usr/freetype/
app/usr/libpng/
app/usr/libjpeg-turbo/
rd/usr/share/fonts/        # bundled TTF files
rd/etc/mario-gui.conf      # simple GUI session configuration
rd/device_list             # adds fb0, psaux, ptmx, pts/0..31
```

Test:

```text
tests/vga/fb0.test
tests/vga/gui.desktop.test
tests/vga/gui.terminal.test
tests/vga/gui.imageviewer.test
tests/vga/gui.filemanager.test
tests/vga/gui.font.test
tests/shell/pty.test
tests/shell/mouse.test     # read-only packet smoke test if device present
```

## Step-by-Step Implementation Guide

This section is the executable form of the plan. Work through the steps in
order. Each step is self-contained enough to build, test, and commit before
the next step. Do not start a later step while an earlier checkpoint is red.

### Guide Conventions

- Files listed under **Change** are the only files expected to change for that
  step. If a needed file is missing from the list, add it and update the list.
- Always run `git diff --check` before committing.
- Run the named shell/VGA test after every kernel/userspace change. A passing
  checkpoint is required before the next step.
- Keep each step in its own commit with the suggested commit subject unless the
  user asks otherwise.
- If a step fails, roll back the local change, make the smallest possible fix,
  and re-run only the failing test before resuming the step.
- All VGA tests run on `tty2`-`tty6`, never `tty1`; `tty1` is the startup
  screen. GUI tests start on `tty2` and switch to `tty7` with F8.
- In steps that build GUI userspace, prefix `make app` and `make image` with
  `GUI=true` so the optional GUI Makefile branch is enabled.
- When a step introduces a new GUI binary, update `app/usr/Makefile` and
  `rd/make-rd0.sh` in that same step so `GUI=true make image` actually
  installs and strips the binary into `ramdisk0`.

Before step 0:

```sh
make shell-test
make vga-test
```

Record the baseline result. Any failure here is pre-existing and must be fixed
or consciously excluded before GUI work begins.

### Step 0: Map the VBE framebuffer and expose `/dev/fb0`

Goal:

- Boot QEMU with a VBE linear framebuffer.
- Map the framebuffer into kernel virtual memory.
- Create `/dev/fb0` with read, write, lseek, and framebuffer info ioctls.

Change:

- `Makefile`: add `GUI ?= false`, export it, and document that `GUI=true`
  enables the userspace GUI build in later steps.
- `src/include/multiboot.h`: add `MB_FLAG_VIDEO`, video type constants, and a
  packed VBE mode info structure.
- `src/kernel/start.S`: set `MB_FLAGS` to `0x00010403` (existing flags plus
  `MB_FLAG_VBE`); append mode type 0, width 1024, height 768, depth 32 after
  the existing Multiboot fields. Keep 1920x1080 as an interactive build-time
  request only after the baseline QEMU test is stable.
- `src/include/video/fb.h`: add `struct fb_info`.
- `src/video/fb.c`: add `fb_early_init()` and `fb_map()`.
- `src/kernel/mario.c` or the current kernel entry file: call `fb_early_init()`
  before paging teardown and `fb_map()` after paging is stable.
- `src/include/fs/chrdev.h`: add `FB_MAJOR 13` and `FB_MINOR 0`.
- `src/fs/char/fb.c`: implement open/lseek/read/write/release/ioctl.
- `rd/device_list`: add `fb0 C 13 0`.
- `tests/vga/fb0.test`: write a known pixel pattern and compare a screendump.

Build and run:

```sh
make kernel
make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test fb0.test tests/vga
```

Checkpoint:

- Kernel `printk` reports a nonzero width, height, stride, bpp, and physical
  base.
- `fb0.test` passes.
- Without VBE information the kernel still falls back to text mode and the
  existing VGA text tests still pass.

Debug:

- If the screen stays black, verify `MB_FLAGS` and the mode word order.
- If the framebuffer is mapped at the wrong address, compare the VBE base
  against the reserved kernel window and add an overlap check.
- If `/dev/fb0` reads garbage, check stride versus width and use
  `bytes_per_scanline` for every row.

Suggested commit:

```text
video: map VBE linear framebuffer and add /dev/fb0
```

### Step 1: Add the graphics console and input devices

Goal:

- Add `/dev/tty7` as the GUI console.
- Keep F1-F6 text consoles unchanged.
- Enable the PS/2 mouse and expose `/dev/psaux`.

Change:

- `src/include/fs/tty/tty.h`: add `CONSOLE_TYPE_GRAPHICS` and a console owner
  notification hook.
- `src/fs/char/tty.c` and `src/fs/char/kbd.c`: register `/dev/tty7`, route F8
  to it, and notify the GUI on switch-away/switch-back.
- `src/include/video/fb.h` and `src/fs/char/fb.c`: add
  `FBIOGET_CONSOLE_STATE` and wake `/dev/fb0` pollers with `POLLPRI` on
  console switches.
- `src/fs/char/psaux.c`: enable the auxiliary PS/2 port, decode the 3-byte
  mouse packet, and implement read/poll.
- `src/include/fs/chrdev.h`: add `PSAUX_MAJOR 10` and `PSAUX_MINOR 1`.
- `rd/device_list`: add `psaux C 10 1`.
- `tests/vga/gui.console.test`: verify F1-F8 switching.
- `tests/shell/mouse.test`: verify `poll()` and packet reads if the mouse is
  present.

Build and run:

```sh
make kernel
make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.console.test tests/vga
.venv/bin/python tests/lit_shell_test.py tests/shell/mouse.test
```

Checkpoint:

- F1-F6 still display the existing text consoles.
- F8 selects the graphics console; F1 returns to text.
- Reading `/dev/tty7` does not echo or apply the normal line discipline.
- `/dev/psaux` produces relative movement/button events or a clean
  keyboard-only fallback when no mouse exists.

Debug:

- If F8 changes the console but the desktop area stays black, ownership
  notification is firing but no GUI process is drawing yet; this is expected
  until step 3.
- If PS/2 initialization hangs, gate the auxiliary-port probe behind a short
  timeout and leave mouse support disabled on failure.

Suggested commit:

```text
console/input: add tty7 graphics console and /dev/psaux mouse
```

### Step 2: Build `libmario-ui` and render TrueType text

Goal:

- Vendor and build FreeType as a static Mario library.
- Implement `gui_surface`, clipping, damage regions, glyph rendering, and a
  CP437 fallback.
- Add a minimal event loop around `poll()`.

Change:

- `app/usr/freetype/`: vendor the pinned source.
- `app/usr/build-freetype.sh` and `app/usr/freetype.mk`: build with
  `util/mario-musl-cc`; enable SFNT/TrueType/CFF and disable unused modules.
- `app/usr/libgui/include/gui/*.h`: define the core types and public API from
  the design section.
- `app/usr/libgui/src/surface.c`: fill, blit, blend, lines, clipping.
- `app/usr/libgui/src/font.c`: FreeType loading, UTF-8 conversion, glyph cache,
  and CP437 fallback.
- `app/usr/libgui/src/event.c`: keyboard/mouse translation and event queue.
- `app/usr/libgui/src/widget.c`: first `gui_label`/`gui_button` widgets.
- `rd/usr/share/fonts/`: bundle a monospace and proportional open-source font.
- `tests/vga/gui.font.test`: render a known string and compare the screen.
- `tests/vga/gui.event.test`: inject keyboard/mouse input and verify a moving
  cursor or button state.

Build and run:

```sh
GUI=true make app
GUI=true make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.font.test tests/vga
.venv/bin/python tests/lit_vga_test.py --test gui.event.test tests/vga
```

Checkpoint:

- A standalone GUI smoke app renders TTF text without crashing.
- The CP437 fallback works when a codepoint is missing from the TTF.
- `gui_surface` conversions produce the same color on 32, 24, and 16 bpp
  fallback modes.
- Keyboard and mouse events are timestamped and delivered in poll order.

Debug:

- If FreeType does not link, run `util/mario-musl-cc -v` on the generated
  library and check for missing musl symbols.
- If glyphs appear as boxes, enable the SFNT/TrueType modules and check the
  UTF-8 decoder.
- If colors are wrong, compare `FBIOGET_VSCREENINFO` masks with the conversion
  table in `surface.c`.

Suggested commit:

```text
libgui: add surfaces, TTF rendering, and event loop
```

### Step 3: Build the single-process desktop and window manager

Goal:

- Create `mario-desktop` with a wallpaper, panel, cursor, and overlapping
  windows.
- Implement move/resize/raise/close/focus and damage-region compositing.

Change:

- `app/usr/desktop/main.c`: application entry and console ownership.
- `app/usr/desktop/wm.c`: z-order, focus, hit testing, move/resize.
- `app/usr/desktop/compositor.c`: dirty-rectangle collection and `/dev/fb0`
  flush.
- `app/usr/desktop/panel.c`: launcher, window list, clock.
- `app/usr/desktop/cursor.c`: pointer tracking and cursor sprite.
- `app/usr/Makefile`: add the desktop binary.
- `tests/vga/gui.desktop.test`: verify panel, wallpaper, cursor, and window
  raise/focus behavior.

Build and run:

```sh
GUI=true make app
GUI=true make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.desktop.test tests/vga
```

Checkpoint:

- F8 shows the desktop; F1 returns to text and F8 back restores the desktop.
- A click raises/focuses a window; Alt-Tab cycles focus; Alt-F4 closes the
  focused window.
- Moving a window redraws only the dirty area well enough to avoid visible
  stale pixels.
- The cursor remains above all application windows.

Debug:

- Stale pixels usually mean damage regions are not clipped through the window
  stack correctly. Dump damage rects behind a debug flag before rewriting the
  compositor.
- If focus changes but the title bar does not, redraw chrome when focus state
  changes rather than only when application content changes.

Suggested commit:

```text
desktop: add single-process window manager and compositor
```

### Step 4: Add Unix98 pseudo-terminals

Goal:

- Refactor the shared tty line discipline.
- Add `/dev/ptmx`, `/dev/pts/N`, master/slave read/write/poll/ioctl, and job
  control signals.

Change:

- `src/include/fs/tty/pty.h`: PTY pair, slave index, size, and status.
- `src/include/fs/tty/tty.h`: extract shared tty/ring buffer interface.
- `src/fs/char/tty.c`: split console-specific code from reusable tty code.
- `src/fs/char/pty.c`: implement Unix98 master/slave operations and cleanup.
- `rd/device_list`: add `/dev/ptmx` and `/dev/pts/N` devices or the `devpts`
  mount path.
- `tests/shell/pty.test`: open ptmx, open slave, run a shell, resize, and send
  Ctrl-C to a foreground process.

Build and run:

```sh
make kernel
make image
make shell-test
.venv/bin/python tests/lit_shell_test.py tests/shell/pty.test
```

Checkpoint:

- The existing shell/VGA console tests still pass after the tty refactor.
- `isatty()` succeeds in the PTY slave.
- `TIOCSWINSZ` from the master changes `stty size` in the shell.
- Ctrl-C from the master delivers `SIGINT` to the foreground process group.
- Closing both ends reuses the lowest-numbered PTY slot.

Debug:

- Run `pty.test` under verbose mode and inspect the master/slave trace before
  touching the tty ring buffer.
- If console input stops working, the refactor removed a console-specific hook;
  keep `tty.c` tests passing before adding PTY features.

Suggested commit:

```text
tty: add Unix98 pseudo-terminals and shared line discipline
```

### Step 5: Add the terminal window

Goal:

- Implement the VT emulator.
- Fork `/bin/oksh` on a PTY slave and connect it to a GUI window.

Change:

- `app/usr/terminal/main.c`: open PTY, fork shell, run event loop.
- `app/usr/terminal/vt.c`: cursor, erase, scroll, SGR, alternate screen, tabs.
- `app/usr/terminal/term.c`: terminal grid, scrollback, resize.
- `app/usr/Makefile`: add the terminal binary.
- `tests/vga/gui.terminal.test`: open a terminal, run commands, resize it, and
  check the screen.

Build and run:

```sh
GUI=true make app
GUI=true make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.terminal.test tests/vga
```

Checkpoint:

- `oksh`, `vim`, `top`, `less`, and interactive Python render acceptably.
- Resizing the window changes `stty size` and redraws the grid.
- Ctrl-C interrupts the foreground process.
- Scrollback survives beyond the visible screen.

Debug:

- If applications do not receive window size, verify `TIOCSWINSZ` is sent to
  the master or slave depending on which fd the shell inherited.
- If `vim` draws incorrectly, add the missing ANSI cursor/erase case in
  `vt.c`; do not add full xterm modes.
- If Ctrl-C does nothing, check that the child called `setsid()` and owns the
  slave as its controlling terminal.

Suggested commit:

```text
terminal: run oksh in a GUI terminal over a PTY
```

### Step 6: Add the image viewer

Goal:

- Vendor and build libpng/libjpeg-turbo as static Mario libraries.
- Decode BMP/PNG/JPEG and implement fit/zoom/pan/next/previous.

Change:

- `app/usr/libpng/` and `app/usr/libjpeg-turbo/`: vendor pinned sources.
- `app/usr/build-libpng.sh`, `app/usr/libpng.mk`,
  `app/usr/build-libjpeg-turbo.sh`, `app/usr/libjpeg-turbo.mk`.
- `app/usr/libgui/src/image.c`: BMP/PNG/JPEG decode to `gui_surface`.
- `app/usr/imageviewer/`: viewer application and image cache.
- `app/usr/Makefile`: add the image viewer binary.
- `tests/vga/gui.imageviewer.test`: open a known BMP and PNG, zoom/pan, and
  check the screen.

Build and run:

```sh
GUI=true make app
GUI=true make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.imageviewer.test tests/vga
```

Checkpoint:

- BMP and PNG open correctly.
- JPEG opens if libjpeg-turbo is available; a decoder failure shows an error
  status instead of closing.
- Fit, 1:1, zoom, pan, and next/previous work.

Debug:

- If PNG links fail, ensure zlib is the same static library used by the rest
  of Mario.
- If colors are inverted, check row order and channel order in the decoder
  conversion.
- If large images exhaust heap, downscale during decode or cap the cache to
  two frames as designed.

Suggested commit:

```text
imageviewer: decode BMP/PNG/JPEG and add zoom/pan
```

### Step 7: Add the file manager

Goal:

- List and navigate directories.
- Open, copy, move, rename, delete, and create directory entries.
- Launch the terminal and image viewer from the file manager.

Change:

- `app/usr/filemanager/`: directory listing, navigation, file operations, and
  launch logic.
- `app/usr/Makefile`: add the file manager binary.
- `tests/vga/gui.filemanager.test`: navigate `/tmp`, create files, rename,
  copy, delete, and launch an image.

Build and run:

```sh
GUI=true make app
GUI=true make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.filemanager.test tests/vga
```

Checkpoint:

- Listing works on mariofs and mounted ext2/vfat filesystems.
- Copy/move/rename/delete/mkdir behave as specified, including confirmation
  before overwrite/recursive delete.
- A text file opens in a terminal, an image opens in the image viewer, and an
  executable runs in a new terminal.
- The view refreshes after every mutation and after child-app exit.

Debug:

- Keep destructive tests in a scratch directory. If a host shared image is
  accidentally changed, restore it from `qemu/make_image.sh` or the image
  build before continuing.
- If launches fail, verify the command line is quoted for `oksh` and that the
  child inherits a valid PTY for terminal launches.

Suggested commit:

```text
filemanager: browse and manage local files from the desktop
```

### Step 8: Split the GUI into multiple processes

Goal:

- Add minimal `AF_UNIX` stream sockets.
- Make `mario-wm` the GUI server and convert applications into clients.

Change:

- `src/include/net/af_unix.h` and `src/net/af_unix.c` or the equivalent Mario
  socket layer: socket/bind/listen/accept/connect/send/recv/socketpair.
- `app/usr/libgui/protocol.c`: length-prefixed message encode/decode.
- `app/usr/desktop/`: split `mario-wm` server from `mario-desktop` client.
- `app/usr/terminal/`, `app/usr/imageviewer/`, `app/usr/filemanager/`: move
  window surface updates behind GUI protocol messages.
- `tests/vga/gui.mp.test`: create a client window, move it, receive input, and
  close it.

Build and run:

```sh
make kernel
GUI=true make app
GUI=true make image
make quick-run-gtk
.venv/bin/python tests/lit_vga_test.py --test gui.mp.test tests/vga
```

Checkpoint:

- Applications run as separate processes.
- Only `mario-wm` opens `/dev/fb0`; clients never receive the framebuffer fd.
- A client disconnect removes its windows and releases the PTY/input resources.
- The single-process Phase 1 build remains available until all Phase 2 apps
  are converted and stable.

Debug:

- Start with `socketpair()` between a parent and child before adding pathname
  sockets; this isolates protocol bugs from filesystem-socket bugs.
- If window updates race, make every message length-prefixed and processed to
  completion before the next `poll()` iteration.
- If a client crash leaves a window behind, verify the server closes the
  connection on `recv()` returning zero or error and cleans up client-owned
  surfaces.

Suggested commit:

```text
gui: move window manager and apps to AF_UNIX multi-process model
```

### Final Integration Checklist

Run the complete test suite:

```sh
make shell-test
make vga-test
```

Run the manual smoke test from the Validation section and confirm all ten
items pass. After that, the GUI plan is complete enough to enter hardening:
measure ramdisk growth, cap font/image caches, and improve the VT emulator
only for the supported apps.

## Validation

Before starting:

```sh
make shell-test
make vga-test
```

After each milestone:

```sh
make kernel
make image
make quick-run-gtk
```

Targeted tests:

```sh
.venv/bin/python tests/lit_vga_test.py --test fb0.test tests/vga
.venv/bin/python tests/lit_vga_test.py --test gui.desktop.test tests/vga
.venv/bin/python tests/lit_vga_test.py --test gui.terminal.test tests/vga
.venv/bin/python tests/lit_vga_test.py --test gui.imageviewer.test tests/vga
.venv/bin/python tests/lit_vga_test.py --test gui.filemanager.test tests/vga
.venv/bin/python tests/lit_vga_test.py --test gui.font.test tests/vga
.venv/bin/python tests/lit_shell_test.py tests/shell/pty.test
```

VGA and input-harness details:

- Run all VGA tests on `tty2` through `tty6`, never on `tty1`. `tty1` is the
  startup screen and may contain boot/shutdown output that is not part of the
  test expectation. GUI tests switch from `tty2` to `tty7` with F8 and back.
- VGA tests run on QEMU `-display gtk` or headless screendump; mouse events are
  injected through the QEMU monitor `mouse_move`/`mouse_button` or a test-only
  `/dev/psaux` writer when needed. The test file declares expected cursor
  positions so runs are deterministic.
- Golden PPM files are generated at 32 bpp `XRGB8888` and normalized by the
  harness before comparison. Small anti-aliasing deltas are allowed by a
  per-test tolerance, but window geometry and solid colors are exact.
- GUI tests must start by sending F8 and waiting for a desktop-ready marker
  (such as a status file or screen pattern) rather than sleeping a fixed time.
- Tests that open PTYs clean up by closing the master/slave and waiting for
  child exit; the harness also kills the QEMU instance so leaked PTYs do not
  affect the next test.
- VGA tests share the same read-only hard-disk images as shell tests. Any
  GUI test that creates files uses a scratch directory under `/tmp` and does
  not modify `/dev/hdc*` images.
- The test harness is extended to dump both the screen and the last `/dev/tty7`
  event trace on failure, to make check-screen failures debuggable.

Manual smoke test:

1. Boot with `-vga std`.
2. Confirm boot starts on F1 text console.
3. Press F8 and confirm the desktop, panel, and cursor appear.
4. Open a terminal and run `vim`, `top`, and `less`.
5. Resize the terminal and confirm `stty size` changes.
6. Open an image and confirm zoom/pan.
7. Browse `/root` and `/mnt` with the file manager.
8. Switch back to F1 and confirm the text console is intact.
9. Boot without VBE and confirm the 80x25 fallback still works.
10. Confirm window titles, panel labels, terminal text, and file-manager
    columns use the bundled TTF font and remain readable at different sizes.

## Risks and Rollback

- Framebuffer MMIO may overlap the current kernel virtual layout. Keep the
  reserved-window conflict check explicit and fall back to text mode on
  failure.
- Existing VGA tests depend on `0xB8000` and 80x25 geometry. Preserve a text
  shadow/regenerate golden images incrementally.
- The custom `boot.bin` path does not yet supply VBE. Do not delete the text
  console path until that loader is upgraded.
- Mouse IRQ 12 can conflict with legacy PIC assumptions. Disable and fall back
  to keyboard-only operation if no mouse is detected.
- PTY work touches the tty line discipline used by all consoles. Keep the
  console path working with the existing shell and VGA tests before adding
  GUI terminal features.
- `AF_UNIX` sockets are a larger kernel change. Keep the single-process GUI
  shippable so socket work does not block the desktop milestone.
- `/dev/fb0` mmap requires physical-device VMA support and must not be
  required by the first GUI. The first implementation uses read/write/pwrite.
- TTF glyph caches can grow quickly at many point sizes. Cap cache entries and
  evict least-recently-used glyphs rather than caching every codepoint.
