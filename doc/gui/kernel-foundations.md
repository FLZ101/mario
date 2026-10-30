# GUI Kernel Foundations

Part of the [GUI platform plan](README.md). This document defines the kernel
services required before userspace can draw and manage windows.

## 1. Request a Multiboot Video Mode

**Ownership boundary:** GRUB4DOS chooses and sets the VBE mode for the image.
The kernel
does not enumerate VBE modes: BIOS interfaces are unavailable and inappropriate
after entering protected mode. The kernel validates the resulting Multiboot
framebuffer record and either maps it or stays in text mode.

In `src/include/multiboot.h`:

- Add the Multiboot framebuffer extension and the packed legacy VBE Mode Info
  Block used by GRUB4DOS.
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

Keep the supported `MB_FLAGS=0x00010003` header in `src/kernel/start.S`.
GRUB4DOS rejects the former `0x00010403` request bit and QEMU's direct
`-kernel` Multiboot loader does not provide VBE. Instead,
`qemu/grub4dos-0.4.6a/menu.lst` uses `setvbe 1024x768x32` before loading the
kernel. GRUB4DOS appends `video=1024x768x32@<physical-base>,<byte-pitch>` to
the Multiboot command line; the kernel validates that form and uses the QEMU
stdvga `xrgb8888` layout. A direct Multiboot framebuffer or legacy VBE record
remains preferred when present.

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

The GUI test runner boots the normal GRUB4DOS disk image. The direct QEMU
`-kernel` and custom `src/boot/boot.asm` paths retain the existing text
fallback until they can provide one of the validated framebuffer handoffs.

EDID/DDC is out of scope. This plan selects the highest VBE BIOS mode rather
than the monitor's native physical mode.

## 2. Capture and Map the Framebuffer

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
3. Use the explicit `0xF0400000` framebuffer window. It is deliberately in a
   different page-directory entry from AHCI's single-slot `ioremap()` aperture
   at `0xF0000000`.
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

## 3. Raw Framebuffer Device `/dev/fb0`

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
- `mmap`: not implemented in the initial GUI ABI. The compositor flushes with
  bounded `pwrite()` calls instead.

Use `verify_area()` and `memcpy_tofs()`/`memcpy_fromfs()` for all user copies.

File semantics:

- `read` and `write` clamp at `fb.size`; they return a short count at the end
  and never wrap file position. Negative seeks and seeks past `fb.size` fail
  with `-EINVAL`.
- `mmap()` returns `-ENOSYS`; no framebuffer mapping is available to
  userspace in the initial GUI ABI. There is no supported mode switch from an
  open framebuffer fd.
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
- `FBIOPUT_VSCREENINFO` returns `-EINVAL` in M0. Add a separate
  `FBIOGET_CONSOLE_STATE` value in M1.
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

- In Phase 1 the GUI process is the only writable opener of `/dev/fb0`;
  normal applications never draw directly to it. Read-only diagnostic opens
  remain allowed.
- In Phase 2 the window-manager server keeps `/dev/fb0` open and never passes
  the fd to clients. Client surfaces are composited by the server.
- Create `/dev/fb0` with root-only permissions (0600 initially). If Mario
  gains a permission system later, add a dedicated `video` group.
- M0 permits its one exclusive raw `/dev/fb0` writer so `fb.test` can prove
  the device ABI without a GUI session. M1 changes ordinary writes to return
  `-EBUSY` unless graphics is foreground; the kernel `fbcon` presenter is not a
  file-operation client and is unaffected by that gate. The desktop redraws
  after switch-back.
- A framebuffer write copies in bounded scanline-sized chunks and rechecks the
  foreground generation between chunks. A switch to text invalidates the old
  generation, stops a later chunk with a short count or `-EBUSY`, and queues a
  newer full text repaint. An in-flight GUI write therefore cannot overwrite a
  restored text VT after the repaint completes.
- Reads are allowed for debugging and screen captures, but they do not grant
  ownership or bypass the switch state.

## 4. Graphics Console and VT Switching

Keep the existing 80x25 text-console cell buffers as the canonical text VT
state. When a framebuffer is enabled, render those cells through a
VGA-compatible 9x16 CP437 text renderer into the largest centered,
aspect-preserving canvas that fits the framebuffer; without one, retain the
existing legacy-VGA output. The renderer uses the legacy 16-color palette and
reproduces the current text cursor and attributes:

This compatibility-console path is an M0 requirement. It initializes before
`/dev/gui`, `mario-desktop`, or any F8 graphics-session support exists: booting
with a valid framebuffer must still show `tty1`, and F1-F6 must switch and
redraw all six text VTs without a desktop process.

```c
console_paint_cell(struct console *con, int row, int col, uint16_t cell);
console_paint_rect(struct console *con, int row, int col, int rows, int cols);
console_paint_cursor(struct console *con);
```

### Framebuffer Text Presenter

Add a small kernel-only presentation backend:

```text
src/include/video/fbcon.h
src/video/fbcon.c
src/video/font_cp437_8x16.inc
```

`fbcon` owns no terminal state. `struct console::mem`, cursor coordinates,
attributes, escape parsing, and F1-F6 selection remain in `console.c`; `fbcon`
only converts an already-final 16-bit cell into framebuffer pixels. This is not
a GUI widget, does not depend on `/dev/fb0` file operations, and is available
before any userspace process starts.

- The checked-in font table is a 256-glyph, one-bit CP437 8x16 bitmap generated
  from the bundled DejaVu Sans Mono source by `util/gen_cp437_font.py`, with
  the source version and license recorded beside the generated file. The
  renderer adds the ninth column needed by the 80x25 VGA-compatible canvas and
  repeats the final glyph column only for CP437 box-drawing characters.
- The presenter uses a 9x18 cell: the 9x16 glyph is vertically centered with
  a two-unit background gap between rows. It scales that grid as a whole, then
  distributes any fractional output pixels across rows and columns. Thus
  `1024x768` uses a centered `1024x640` canvas and `1920x1080` uses
  `1728x1080`, while terminal applications continue to observe an unchanged
  80x25 geometry.
- Scaled bitmap glyphs use fixed-point bilinear coverage to blend their
  foreground and background palette colors, smoothing enlarged text without
  changing the CP437 glyph set, cell layout, or box-drawing extension.
- `fbcon` maps the existing 4-bit foreground/background attributes through the
  canonical VGA 16-color palette. It writes directly through `fb_info` using
  the actual stride and pixel masks; it never assumes XRGB8888 outside the
  fast path.
- In framebuffer mode the cursor is software-rendered. Repaint the old cursor
  cell from `con->mem`, then paint the new cursor cell with the cursor overlay.
  `show_cursor`, `hide_cursor`, and `move_cursor` must therefore dispatch to
  `fbcon` instead of programming the VGA CRTC cursor registers. The initial
  cursor is a non-blinking, two-pixel-high inverse-color underline; blinking is
  deferred so screenshot output is deterministic. `CSI ?25l` hides the
  underline and `CSI ?25h` restores it.
- On the legacy path, retain the current `VIDEO_MEM` and hardware-cursor
  behavior unchanged. `console_sync()` imports `0xB8000` only on that path;
  in framebuffer mode `con->mem` is already authoritative.
- `printk_init()` prepares `tty1` before device initialization and records
  early kernel output in its cell buffer. A framebuffer boot preserves that
  prepared buffer when `console_init()` registers the tty devices, so F1 shows
  the kernel boot text once `fbcon` performs its first repaint. GRUB4DOS menu
  text cannot be preserved because `setvbe` replaces the bootloader text mode
  before the kernel starts.

Refactor the current direct `VIDEO_MEM` mirrors into one presentation boundary:

- normal output and erase operations present a changed cell or cell span;
- insert/delete operations repaint the affected tail of one row;
- scrolling repaints the changed row range;
- clear, alternate-screen changes, and F1-F6 selection present the whole
  80x25 canvas.

Do not repaint the whole framebuffer for every character. A console update
marks the smallest changed cell rectangle and cursor cells while holding
`console_lock`, then wakes the one `fbcon` kernel thread. That thread starts
after `sched_init_smp()`, sleeps on an `fbcon_wait` queue, and is the only
context that writes framebuffer text pixels. It snapshots the marked cells and
display generation under `console_lock`, releases the lock, then writes pixels.
Updates that arrive during a paint remain dirty and are drained by the same
thread before it sleeps again; a foreground-console switch increments the
generation and forces a full snapshot. This prevents stale paints without
doing large linear-framebuffer copies while holding `console_lock`. Do not use
the current bottom-half mechanism for this work: it runs with interrupts
disabled. `fbcon_start()` marks the current `tty1` foreground state fully dirty
before waking the thread, so the first VBE frame is the text console rather
than uninitialized scanout memory.

Writes to a non-foreground text VT update only that VT's `con->mem`; they do
not wake `fbcon`. Selecting that VT later forces the full snapshot, so background
getty or shell output is preserved without repainting the visible framebuffer.

The same worker serializes foreground display transitions. F8 changes input
routing to `FOREGROUND_GRAPHICS`, queues a black scanout clear, and emits the
`/dev/fb0` `POLLPRI` notification only after that clear completes; the desktop
then composes its first frame. F1-F6 gate user framebuffer writes immediately,
queue the selected text VT's full repaint, and emit `POLLPRI` after that repaint
completes. Thus no transition exposes a stale text frame as desktop content or
lets a stale desktop write survive the restored text frame.

Console layout:

- `/dev/tty1` through `/dev/tty6`: existing text consoles.
- graphics console: a foreground display/input-routing state with no tty node.
  The desktop receives keyboard events through `/dev/gui`.
- serial: existing serial consoles.

Add:

```c
enum foreground_console {
    FOREGROUND_TEXT,
    FOREGROUND_GRAPHICS,
};

void switch_text_console(unsigned int index);
int enter_graphics_console(void);
```

Keep `fg_text_console` as an index in `0..5` and store the independent
`foreground_console` mode beside it. `switch_text_console()` updates the text
index, selects `FOREGROUND_TEXT`, resets text-keyboard state, and requests an
`fbcon` full redraw. `enter_graphics_console()` leaves `fg_text_console`
unchanged, requires a valid framebuffer plus the registered `/dev/gui` owner,
then selects `FOREGROUND_GRAPHICS`; it returns `-ENODEV` or `-EBUSY` rather
than overloading a text-console index. The legacy `switch_fg_console()` name
may remain as a wrapper around `switch_text_console()` while existing callers
are converted.

`switch_fg_console()` behavior:

- F1-F6 selects a text console and redraws its 80x25 cell buffer through the
  framebuffer text renderer when available, otherwise through legacy VGA.
- F8 selects the graphics console only when `fb_info.enabled` is true and the
  exclusive `/dev/gui` owner is registered. The kernel does not draw text into
  it; without a framebuffer or GUI owner, F8 is a no-op.
- F7 and F9-F12 are reserved for future kernel console actions; initially they
  leave the foreground console unchanged.
- Switching away from F8 notifies the GUI process that it no longer owns the
  framebuffer; switching back notifies it to redraw.
- Add `FBIOGET_CONSOLE_STATE` to `/dev/fb0`. It returns `0` while a text
  console is foreground and `1` while the graphics console is foreground.
- `poll()` on `/dev/fb0` wakes with `POLLPRI` when the state changes. The GUI
  event loop treats `POLLPRI` as a console-change event, calls
  `FBIOGET_CONSOLE_STATE`, and does a full desktop redraw when state returns
  to `1`.
- `/dev/gui` is a read-only graphics-input endpoint, not a terminal. It does
  not expose termios or the tty line discipline, and is not associated with a
  tty minor. All F1-F12 keys remain kernel-owned and are not delivered as
  ordinary input.
- The graphics mode remembers the last selected text console. If the `/dev/gui`
  owner or framebuffer writer closes, the kernel clears pending GUI and mouse
  events, restores that text console, and emits `POLLPRI` before releasing
  either device owner. The desktop must close both fds during normal shutdown.

`is_fg(con)` is true only when `foreground_console == FOREGROUND_TEXT` and
`con` is `fg_text_console`. The PS/2 keyboard path handles F1-F12 before it
looks up a text tty: in graphics mode non-reserved keys go only to `/dev/gui`,
and the last text tty must not receive them accidentally.

Model the ownership transition on Linux `KD_GRAPHICS`/`KD_TEXT`.

## 5. Keyboard Input

The existing `src/fs/char/kbd.c` already decodes PS/2 keyboard bytes for text
consoles. The GUI adds a separate fixed-record input queue for `/dev/gui`;
the keyboard IRQ routes events to that queue only while the graphics console
is foreground. It must not feed GUI keys through the tty line discipline.

`read()` from `/dev/gui` returns only complete records in this Mario ABI:

```c
struct gui_key_event {
    uint16_t scancode; /* set-1 code; extended codes have bit 8 set */
    uint8_t pressed;   /* 1 for make, 0 for break */
    uint8_t flags;
    uint32_t ticks;    /* monotonic kernel tick captured at IRQ time */
} __attribute__((packed));
```

`flags & GUI_KEY_EVENT_OVERFLOW` means one or more older records were dropped
before this record. `GUI_KEY_EVENT_OVERFLOW` is `0x01`; all other flag bits are
zero in the first ABI.

`read()` with a buffer smaller than one record returns `-EINVAL`; nonblocking
empty reads return `-EAGAIN`; `poll()` reports `POLLIN` while records are
queued. `/dev/gui` permits one `O_RDONLY` opener; a concurrent open returns
`-EBUSY`, and writable opens return `-EACCES`. When graphics mode is not
foreground, the kernel does not enqueue new GUI key events and the desktop
resets its modifier/repeat state after observing the `/dev/fb0` `POLLPRI`
notification. The queue holds 256 records. On overflow, the driver drops the
oldest record, marks the newly queued record with `GUI_KEY_EVENT_OVERFLOW`,
and never blocks the IRQ path; userspace clears its key/modifier state before
handling that record. Userspace owns modifier tracking, auto-repeat, and
conversion to the small `keysym` set used by `libmario-ui`: printable characters, arrows,
Home/End, Page Up/Down, Insert/Delete, Shift, Ctrl, Alt, and Super. A future
`/dev/input/event0` interface is optional and must preserve the reservation of
all function keys.

Keyboard translation details:

- Userspace maintains a small scancode table with modifier state. Auto-repeat
  starts after a 500 ms delay and then repeats at 30 Hz while the key is held;
  the repeat policy lives in `libmario-ui`, not the kernel.
- The initial keysym set is enough for ASCII input plus arrows, Home/End, Page
  Up/Down, Insert/Delete, Shift, Ctrl, Alt, and Super. Caps Lock toggles an
  internal modifier flag.
- F1-F12 are consumed by the kernel before GUI input routing. F1-F6 and F8
  select foreground consoles; F7 and F9-F12 are reserved no-ops initially.
  None appear as `gui` key events, including with modifiers held.
- Unmapped scancodes are ignored but logged in a bounded debug ring; they must
  not wedge the input stream.

## 6. Mouse Input

Mouse support is required for a useful window manager and file manager. The
current `ps2_init()` disables the PS/2 auxiliary port; this must be changed.

Kernel work:

- Enable the PS/2 controller auxiliary device and IRQ 12.
- Decode the standard 3-byte PS/2 mouse packet:
  - left/middle/right buttons;
  - signed X/Y deltas;
  - `dz` fixed to zero in the first ABI.
- Expose relative movement only through `/dev/psaux` in the first GUI
  milestone; `/dev/input/mouse0` and IntelliMouse wheel negotiation are
  deferred.
- Make the device `read()`/`poll()` compatible so the GUI event loop can wait
  on both keyboard and mouse fds.

Userspace work:

- Accumulate relative deltas with no acceleration: `x = clamp(x + dx)` and
  `y = clamp(y - dy)`, because PS/2 positive Y means up while GUI coordinates
  increase downward. The pointer starts at the framebuffer center when the
  desktop starts and retains its position across text/graphics switches.
- Convert button snapshots into press and release events.
- Render a cursor sprite as the topmost compositor layer.

The initial implementation only supports one pointer. Multi-touch is out of
scope.

`/dev/psaux` read format:

```c
struct psaux_event {
    int16_t dx;
    int16_t dy;
    int8_t  dz;      /* always 0 in the first ABI */
    uint8_t buttons; /* bit 0 left, bit 1 right, bit 2 middle */
    uint8_t flags;
    uint8_t reserved;
    uint32_t ticks;  /* monotonic kernel tick captured at IRQ time */
} __attribute__((packed));
```

- This is Mario's stable mouse-event ABI, not the Linux byte-stream `psaux`
  protocol. Each `read()` returns one complete 12-byte event or `EAGAIN` when
  `O_NONBLOCK` and no event is pending. Smaller reads return `-EINVAL`;
  partial reads never occur.
- `/dev/psaux` permits one `O_RDONLY` opener. `poll()` reports `POLLIN` when
  at least one event is available and `POLLHUP` if the auxiliary device is
  absent or fails. The GUI runs keyboard-only in that case.
- The queue holds 256 events. On overflow, the driver drops the oldest record
  and sets `PSAUX_EVENT_OVERFLOW` (`0x01`) in the newly queued event; the
  desktop clears its button state before handling that event. The driver never
  blocks the IRQ path or exposes a half-packet.

## 7. Pseudo-Terminals

The standalone PTY and tmux work is implemented and specified in
`doc/PTY_TMUX_PLAN.md`. That document owns the Unix98 ABI, tty/session
semantics, package builds, and acceptance tests. GUI work consumes that stable
contract; it does not add a second PTY driver or change the tty ABI.

The GUI terminal must use a real tty. The earlier pipe-based shell was a
prototype only; job control, `isatty()`, `TIOCSWINSZ`, and terminal signal
delivery need a PTY.

The GUI terminal uses the existing Unix98 PTY model:

- `/dev/ptmx` is the cloning master.
- `/dev/pts/N` are slave devices.
- The master and slave are paired file objects.
- Use static `/dev/ptmx` and `/dev/pts/0..31` device nodes in `rd/device_list`
  rather than introducing a separate `devpts` filesystem or mount point in
  the first implementation. This keeps the device namespace consistent with
  Mario's existing static `/dev` layout.

Terminal startup follows `posix_openpt()`/`grantpt()`/`unlockpt()`/`ptsname()`:
the terminal opens the master, unlocks the selected slave, and forks. The
child calls `setsid()`, opens the slave, makes it its controlling tty, maps it
onto standard input/output/error, then execs `/bin/oksh`. The parent retains
only the master. On every terminal resize, the parent sends `TIOCSWINSZ` to
the master and updates its local grid before the next repaint.

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
- A newly allocated slave is locked. The terminal unlocks it before the child
  opens the slave; it does not relock a live terminal.
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

## 8. GUI Inter-Process Communication

Mario already provides the required `AF_UNIX/SOCK_STREAM`, `socketpair`,
`poll`, and one-FD `SCM_RIGHTS` transport. The window manager protocol uses
that implementation directly; do not create a GUI-private socket layer.
Datagrams and abstract names remain out of scope. The first single-process
desktop still ships independently, but the multi-process phase is no longer
blocked on kernel socket work.

GUI protocol message families:

- `WINDOW_CREATE`/`WINDOW_DESTROY`;
- `WINDOW_MOVE`/`WINDOW_RESIZE`;
- `WINDOW_SET_TITLE`;
- `SURFACE_UPDATE` carrying a dirty rectangle and its pixels;
- `INPUT_EVENT`;
- `WINDOW_FOCUS`/`WINDOW_RAISE`;
- `APP_READY`/`APP_EXIT`.

The server owns the framebuffer and input devices. Clients never write to
`/dev/fb0` directly in the multi-process model.

Socket and message details:

- Phase 2 server creates `/tmp/.mario-wm` itself with mode `0700`, validates
  that it is a root-owned directory, and listens on
  `/tmp/.mario-wm/socket` with mode `0600`. At startup it removes only a stale
  socket owned by root; any other existing path is a fatal startup error.
- Backlog is fixed at 8. Reject additional connects with `ECONNREFUSED` until
  a client disconnects.
- The socket namespace is ordinary filesystem pathnames only; no Linux
  abstract-socket namespace because Mario's pathname lookup does not support it.
- Every message starts with this packed little-endian header; `length` includes
  the header and must be between 8 and 65536 bytes:

  ```c
  struct wm_message_header {
      uint32_t length;
      uint16_t type;
      uint16_t flags;
  } __attribute__((packed));
  ```

  Receivers buffer stream bytes until a whole message is available, reject an
  invalid length or nonzero header flags with `EPROTO`, and process at most
  eight complete messages from one client before returning to `poll()`.
- Before implementing clients, add `include/gui/protocol.h` with a protocol
  version constant, a complete message-type enum, and packed payload structs.
  `WINDOW_CREATE` carries a client request ID, dimensions, and a length-bounded
  UTF-8 title; its reply returns that request ID plus the server-assigned
  window ID and accepted dimensions. Every later client message begins with
  that window ID. The server rejects an unknown window ID or one owned by
  another client with `EPROTO` and disconnects the sender.
- `SURFACE_UPDATE` carries a dirty rectangle and uncompressed premultiplied
  `ARGB8888` pixel rows. Its packed payload starts with four little-endian
  `uint32_t` values: `window_id`, `x`, `y`, and `width`, followed by
  `uint32_t rows`; the remaining bytes are exactly `width * rows * 4` pixels.
  The server requires the rectangle to be within the window surface. Each
  update must fit the 64 KiB bound; larger rectangles are split into
  top-to-bottom row-aligned chunks, sent contiguously by a client, and applied
  in message order. All wire payloads use fixed-width little-endian fields;
  no raw C structs containing pointers, `size_t`, or `gui_event` are sent.
  RLE is deferred until profiling shows socket copies dominate.
- GUI messages do not use `SCM_RIGHTS` in the first multi-process phase.
  Clients never receive `/dev/fb0`; a later file-picker protocol may pass
  ordinary file descriptors only after explicit lifetime tests.
- On disconnect, the server releases all window surfaces, input grabs, and
  file descriptors associated with that client.
