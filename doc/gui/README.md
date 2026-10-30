# GUI Platform Plan

This directory replaces `doc/FULL_RESOLUTION_GRAPHICS_PLAN.md` with focused
documents that can be implemented and reviewed independently:

- [Kernel foundations](kernel-foundations.md): Multiboot/VBE handoff, `/dev/fb0`,
  graphics-console switching, input, PTYs, and GUI IPC.
- [UI toolkit](ui-toolkit.md): `libmario-ui`, surfaces, drawing, fonts, widgets,
  events, and image decoding.
- [Desktop and applications](desktop.md): compositor, terminal, image viewer,
  file manager, launcher, and proposed source layout.
- [Implementation roadmap](roadmap.md): ordered checkpoints, validation, risks,
  and rollback guidance.


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
Pressing the F8 graphics-console key switches to the GUI, while
F1-F6 continue to switch back to text consoles. All F1-F12 keys remain
kernel-reserved: F7 and F9-F12 are currently consumed without changing the
foreground console.

## Success Criteria

Measurable checks, in order of difficulty:

- `qemu-system-i386` with `-vga std` reports and maps a VBE linear framebuffer
  rather than only the legacy text plane.
- `/dev/fb0` reports the real width, height, stride, and depth through
  `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`.
- A `gui-test` case writes a known pixel pattern to `/dev/fb0` and compares a
  QEMU `screendump` against a golden PPM.
- Before any desktop process is started, F1-F6 switch between the existing
  text virtual consoles rendered through the framebuffer; F8 later switches
  to the GUI and back without losing either mode.
- `/dev/ptmx` can create a PTY, `/dev/pts/N` behaves as a tty, and `isatty()`
  plus `TIOCSWINSZ` work for a shell running inside a terminal window.
- A desktop process draws a fixed solid background, panel, cursor, and
  overlapping windows.
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

1. **Framebuffer platform:** boot reliably in a known VBE mode, render and
   switch the text consoles before any desktop starts, expose a safe
   framebuffer/input ABI, and prove pixels with automated screenshots.
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
  a real-mode VBE mode list. The common image always accepts a valid
  direct-color framebuffer handoff and enables the framebuffer path.
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
- Mario builds one image containing the VBE request, framebuffer/input
  drivers, GUI binaries, fonts, and test fixtures. F1-F6 versus F8 is a
  runtime foreground-console choice, never a build or image choice.
- If the loader supplies a valid framebuffer, text VTs are rendered from their
  existing 80x25 cell buffers with a VGA-compatible 9x16 CP437 bitmap centered
  in a 9x18 cell, leaving a two-unit line gap. The largest aspect-preserving
  canvas is used while F1-F6 is foreground and the remaining scanout pixels
  are black. This kernel-only presenter uses a fixed bitmap font, not FreeType
  or any desktop/UI component. If no valid framebuffer is supplied, the same
  image retains the legacy hardware text path and F8 remains unavailable.

No phase may start until its entry conditions are met, and it is complete only
when its listed test gate passes. Unix98 PTYs and the minimal `AF_UNIX` stream
transport are existing prerequisites; GUI work integrates them rather than
reopening their kernel ABI. Do not start FreeType or a multi-process protocol
before the framebuffer platform has a stable, screenshot-tested ABI.

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
  writer. A second `O_WRONLY` or `O_RDWR` open fails with `-EBUSY`; framebuffer
  `mmap()` is not part of the initial ABI. A later capability/seat model may
  relax this rule; it is not part of phase 0.
- The compositor is the only process that writes scanout pixels in GUI mode.
  Clients never receive a direct `/dev/fb0` descriptor.
- Raw input is routed to exactly one foreground consumer: the selected text
  VT or `mario-desktop` through `/dev/gui`. The kernel retains all F1-F12
  handling before routing the remaining key event, so a wedged desktop cannot
  trap the user in graphics mode.
- `/dev/gui` is an exclusive GUI seat and input stream, not `/dev/tty7` and
  not a framebuffer alias. Only the desktop/window-manager owner may open it;
  a second opener receives `-EBUSY`. Closing the owner forces the foreground
  back to `tty1`, drops GUI input state, and makes F8 unavailable until another
  owner registers.
- F1-F6 select text VTs, F8 enters graphics only while that GUI seat is owned,
  and F7/F9-F12 are reserved no-ops. These keys are consumed on key press and
  are never delivered to text or GUI clients, including with modifiers.
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
- **zlib** for decompression used by PNG.
- **pixman** only after profiling demonstrates that custom compositing is the
  bottleneck.

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

- Target no more than roughly 1.6 MiB of additional stripped, statically linked
  GUI support in the first ramdisk: about 600 KiB for FreeType, 250 KiB for
  libpng plus zlib, and 700 KiB for `libmario-ui` and the initial desktop
  applications combined. These are planning ceilings,
  not correctness limits; if they are exceeded, trim unused FreeType modules
  before reducing features.
- Keep all generated binaries static and `strip` them in the image build.
- Measure the ramdisk delta in `make image`; the GUI payload is always staged
  in the single image once its milestone is implemented. If the budget is
  exceeded, trim unused FreeType modules before reducing features.
- `make app` always builds the GUI libraries/apps once introduced, and
  `rd/make-rd0.py` always copies their outputs into the common ramdisk.
- Target `_LIBC=musl` first because the GUI libraries and FreeType/libpng
  builds are validated with `util/mario-musl-cc`. Custom-libc
  builds are a follow-up, not an M2 prerequisite.

Version and license baseline:

- Pin versions in the `.mk` files at adoption time. Use the table below as the
  expected baseline, updating it when a different version is actually vendored.
- FreeType: pin one exact archive version, download URL, and SHA-256 in
  `app/usr/freetype.mk` before recording `gui.font.test` goldens; use FTL
  terms. Updating that pin requires regenerating and reviewing all font-based
  GUI goldens.
- libpng: expected baseline 1.6.x; libpng license.
- zlib: reuse the version already vendored by Mario; zlib license.

Fallback policy:

- If FreeType cannot be built for musl/i386, M2 is blocked; do not swap in
  a second rasterizer after the visual-test baseline exists.
- If libpng cannot be built, make the image viewer degrade gracefully: BMP
  always works; PNG shows an explicit unsupported-format message rather than
  failing to launch. JPEG decoding is deferred beyond the first GUI milestone.

## Desktop and Window Manager Alternatives

Suitable open source alternatives exist, but none drops cleanly onto Mario
because Mario has no X11/Wayland protocol, no GUI runtime, and no dynamic
linking.

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

Baseline decision:

- Implement the phase-1 widgets in `libmario-ui`; only labels, buttons, lists,
  menus, and text fields required by the first applications are in scope.
- Keep the Mario window manager/compositor small and custom because it must
  own the framebuffer, route `/dev/gui` and `/dev/psaux` events, and integrate
  with Mario's VT switching and PTY model.
- Do not vendor LVGL in the initial GUI milestone. Re-evaluate it only after
  the custom widget API and framebuffer benchmarks are stable; Microwindows
  remains an architecture reference, not an implementation dependency.

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
  | /dev/fb0 | /dev/gui | /dev/ttyN | /dev/psaux | /dev/ptmx, /dev/pts/N |
  +-----------------------------------------------------------+
  | console switch + framebuffer map + input IRQ + tty core    |
  +-----------------------------------------------------------+
```

The first GUI milestone can run the desktop and applications in one process to
limit compositor and application complexity. The final target is
multi-process: the window manager is the GUI server and each application is a
client. Mario's existing `AF_UNIX` stream-socket transport is the planned IPC
mechanism because it is a natural fit for event messages and can later pass
file descriptors with `SCM_RIGHTS` if needed.
