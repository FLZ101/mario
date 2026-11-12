# GUI Kernel Foundations

Part of the [GUI platform plan](README.md). This document defines the kernel
services required before userspace can draw and manage windows.

## Scope and Authority

This is the normative contract for kernel-owned GUI behavior: boot handoff
validation, UAPI structure layout, device ownership, syscall semantics,
foreground transitions, and synchronization. Userspace documents may depend
on these interfaces but must not redefine them. The
[implementation roadmap](roadmap.md) owns implementation order, completion
state, and test/commit sequencing; update it when this contract adds a new
prerequisite or validation requirement.

### Kernel/Userspace Handoff

The availability of each interface is staged and must be consumed only after
its owning milestone is green:

| Interface | First consumer | Contract owner |
| --- | --- | --- |
| `/dev/fb0` metadata, bounded I/O, and framebuffer text VTs | M0 probes and text tests | this document |
| `/dev/gui`, `/dev/psaux`, F8, `FBIOGET_CONSOLE_STATE`, and `POLLPRI` | M1 graphics-seat tests | this document |
| `pread64()`/`pwrite64()` framebuffer semantics | M1.9 positional probe, then M2/M3 flushing | this document |
| Unix98 PTY behavior | M4 gate and M5 terminal | `doc/PTY_TMUX_PLAN.md` |
| `AF_UNIX` stream transport | M3 window-manager protocol | this document |

An application must not infer an interface from a nearby implementation detail
or use a future fallback path early. In particular, direct framebuffer mapping,
per-client framebuffer access, and GUI-private socket transports are outside
this contract.

## 1. Request a Multiboot Video Mode

**Ownership boundary:** GRUB4DOS chooses and sets the VBE mode for the image.
The kernel does not enumerate VBE modes: BIOS interfaces are unavailable and
inappropriate after entering protected mode. It validates one of the supported
Multiboot/VBE/GRUB4DOS handoffs and either maps it or stays in text mode.

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

The GRUB4DOS command-line fallback is a stable, deliberately narrow Mario boot
ABI, not a generic `video=` option parser:

- Match `video=` only at the beginning of the command line or immediately after
  ASCII whitespace. A substring such as `novideo=` must not match.
- Accept exactly decimal `widthxheightxbpp`, `@0x` followed by hexadecimal
  physical base, a decimal comma-separated pitch, and then end-of-token.
  Reject signs, omitted fields, trailing garbage, and additional `video=`
  tokens rather than silently accepting the first ambiguous value.
- Parse every field with checked arithmetic before narrowing it. The resulting
  base, pitch, dimensions, and derived mapping range pass the same validation
  as a Multiboot framebuffer record.
- Keep this fallback below direct Multiboot framebuffer and legacy VBE records
  in priority order. Tests cover all three accepted handoffs and malformed
  command-line values that must leave Mario in text mode.

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
4. If no graphics mode was selected and VBE data is absent, leave
   `fb_info.enabled == 0` and continue with legacy text. If a loader already
   selected VBE but its handoff is invalid, leave the framebuffer disabled and
   report the fault through serial; protected mode does not attempt an unsafe
   legacy-VGA mode restore.

Validation and mapping rules:

- Accept only direct-color/packed-pixel framebuffer records with nonzero base,
  width, height, stride, and bpp. Reject text-mode, planar, banked-only, and
  palette-only records in M0.
- Compute `size = stride * height` with checked 64-bit arithmetic. Reject a
  size of zero, a size above the reserved framebuffer virtual-window capacity,
  or an address range that wraps `phys_base + size`.
- Mario is an i386 kernel: reject a direct Multiboot physical base with any
  nonzero high 32 bits, and reject a page-aligned end above the address range
  that `fb_map()` can represent. Also reject a handoff that overlaps usable RAM
  or a boot module instead of treating ordinary reclaimable memory as MMIO.
- Require `stride >= ceil(width * bpp / 8)`. Reject color fields whose length
  is zero, whose offset plus length exceeds `bpp`, or whose red, green, and blue
  bit ranges overlap. The accepted direct-color layout is reported verbatim;
  only the GRUB4DOS QEMU fallback supplies its fixed XRGB8888 fields.
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
- `ioctl`: implement `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO`; return
  `-EINVAL` for unrecognized or mode-setting requests because the initial ABI
  exposes no mode-setting ioctl.
- `mmap`: not implemented in the initial GUI ABI. The compositor flushes with
  bounded `pwrite()` calls instead.

`pwrite64()` is an explicit prerequisite for the desktop compositor, not an
assumption supplied by libc. Before M3, implement and test `pread64()` and
`pwrite64()` in the i386 syscall table. Their positional file-operation path
must not change the shared file description's `f_pos`, must retain normal
user-copy/error semantics, and must not implement positional I/O by a visible
`lseek()`/I/O/restore sequence. M0's simple framebuffer probe may use
`lseek()` plus `read()`/`write()` until this gate is complete.

Add explicit `read_at`/`write_at` file-operation callbacks which receive a
caller-local `off64_t` and advance only that local value. Regular `read()` and
`write()` use the same underlying helpers with `f_pos`; `pread64()` and
`pwrite64()` call only the positional callbacks. Non-seekable devices return
`-ESPIPE`. Decode the i386 low/high offset pair with checked signed 64-bit
arithmetic, reject negative/out-of-range offsets before user access, and route
framebuffer positional writes through the same foreground-generation gate as
ordinary writes.

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

- Define Mario's fixed-width `FBIOGET_VSCREENINFO` and
  `FBIOGET_FSCREENINFO` payloads in `src/include/uapi/mario/fb.h`, copied into
  the userspace include tree as `mario/fb.h`. The source spellings
  `struct fb_bitfield`, `struct fb_var_screeninfo`, and
  `struct fb_fix_screeninfo` are conventional names only; programs must use
  Mario's installed header rather than assume binary compatibility with a host
  Linux header. Kernel-private framebuffer state remains outside that header.
- `struct fb_bitfield` is exactly 8 bytes. `struct fb_var_screeninfo` is
  exactly 144 bytes: it contains visible/virtual geometry, offsets, bpp,
  grayscale, four color bitfields, timing/mode metadata, and four reserved
  words. `struct fb_fix_screeninfo` is exactly 72 bytes: it contains `id[16]`,
  a `uint64_t smem_start`, memory/type/visual metadata, panning steps, line
  length, MMIO metadata, acceleration, capabilities, and reserved words.
- M0 publishes only the three defined getters:
  `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, and, after M1,
  `FBIOGET_CONSOLE_STATE`. Unrecognized or mode-setting ioctl values return
  `-EINVAL`; no `FBIOPUT_VSCREENINFO` request is published in the ABI.
- Keep these ioctl numbers stable in the header; changing them after a
  userspace build would break the GUI binaries.
- Use fixed-width integer fields and compile-time size assertions in both
  kernel and userspace builds. Programs include this shared header rather than
  duplicating framebuffer definitions locally. The Linux-looking ioctl and
  structure names are compatibility names, not a promise that Linux headers
  can be used unchanged.

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

- From M3 onward `mario-wm` is the only writable opener of `/dev/fb0`; normal
  applications never draw directly to it. Read-only diagnostic opens remain
  allowed. The server never passes the fd to clients; client surfaces are
  composited by the server.
- Create `/dev/fb0` with root-only permissions (0600 initially). If Mario
  gains a permission system later, add a dedicated `video` group.
- M0 permits its one exclusive raw `/dev/fb0` writer so `fb.test` can prove
  the device ABI without a GUI session. M1 changes ordinary writes to return
  `-EBUSY` unless graphics is foreground; the kernel `fbcon` presenter is not a
  file-operation client and is unaffected by that gate. The desktop redraws
  after switch-back.
- M1.10 replaces the initial single-copy write path with bounded
  scanline-sized chunks that recheck the foreground generation between chunks.
  A switch to text then invalidates the old generation, stops a later chunk
  with a short count or `-EBUSY`, and queues a newer full text repaint. M2/M3
  must not rely on a large-write transition guarantee until M1.10.3 is green.
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
src/video/font_cp437_18x32_gray.inc
```

`fbcon` owns no terminal state. `struct console::mem`, cursor coordinates,
attributes, escape parsing, and F1-F6 selection remain in `console.c`; `fbcon`
only converts an already-final 16-bit cell into framebuffer pixels. This is not
a GUI widget, does not depend on `/dev/fb0` file operations, and is available
before any userspace process starts.

- The checked-in font table is a 256-glyph, 18x32 grayscale CP437 bitmap
  supersampled from the bundled DejaVu Sans Mono source by
  `util/gen_cp437_font.py`, with the source version and license recorded
  beside the generated file. The generator stores the extra two source columns
  needed by the 80x25 VGA-compatible canvas and repeats the final glyph columns
  only for CP437 box-drawing characters.
- The presenter uses a 9x18 cell: the 9x16 glyph is vertically centered with
  a two-unit background gap between rows. It scales that grid as a whole, then
  distributes any fractional output pixels across rows and columns. Thus
  `1024x768` uses a centered `1024x640` canvas and `1920x1080` uses
  `1728x1080`, while terminal applications continue to observe an unchanged
  80x25 geometry.
- Scaled bitmap glyphs use fixed-point bilinear coverage on the grayscale
  source to blend their foreground and background palette colors, smoothing
  enlarged text without changing the CP437 glyph set, cell layout, or
  box-drawing extension.
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

The M0 presenter uses one explicit presentation boundary. A console write
updates its normal cell buffer while holding `console_lock`, releases that
lock, and then wakes the `fbcon` worker if the changed console is foreground.
The worker sleeps on `fbcon_wait`, snapshots the complete foreground 80x25
buffer and cursor under `console_lock`, releases the lock, then compares that
snapshot with its previous one. It repaints changed cells and cursor changes;
a foreground-VT switch forces a full scanout repaint. This avoids framebuffer
pixel writes under `console_lock`, but it deliberately scans the 80x25 cell
grid rather than maintaining dirty rectangles in M0. Do not use the current
bottom-half mechanism for this work: it runs with interrupts disabled.

`fbcon_start()` starts after the init task has been created so init retains PID
1. It queues the first full repaint after the worker starts; the first VBE
frame is therefore the prepared text console rather than uninitialized scanout
memory. M0 currently relies on successful worker creation. The required
failure policy is fail-closed: once the kernel has selected the framebuffer
text path, an inability to create its sole presenter prints one concise serial
diagnostic and halts the kernel. It must not attempt legacy-VGA restoration or
a synchronous fallback presenter. M1.12 adds a checked worker-spawn result and
an expected-halt regression for this path.

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
- Switching away from F8 preserves the GUI process's open descriptors but
  gates framebuffer writes and input delivery while text is foreground.
  Switching back reports the completed transition so the process redraws.
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
- The graphics mode remembers the last selected text console. When the final
  reference to either exclusive `/dev/gui` or writable `/dev/fb0` open file
  description closes, the kernel clears pending GUI and mouse events, restores
  that text console, and emits `POLLPRI` before releasing either device owner.
  `dup()` and `fork()` references therefore retain the seat; closing only one
  duplicate must not switch displays. The desktop closes both original fds
  during normal shutdown and must not leak them into child applications.

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

`read()` from `/dev/gui` returns only complete records defined in the shared
`src/include/uapi/mario/input.h` header, installed for userspace as
`mario/input.h`:

```c
struct mario_gui_key_event {
    uint16_t scancode; /* set-1 code; extended codes have bit 8 set */
    uint8_t pressed;   /* 1 for make, 0 for break */
    uint8_t flags;
    uint32_t ticks;    /* monotonic kernel tick captured at IRQ time */
};
```

`flags & GUI_KEY_EVENT_OVERFLOW` means one or more older records were dropped
before this record. `GUI_KEY_EVENT_OVERFLOW` is `0x01`; all other flag bits are
zero in the first ABI.

`sizeof(struct mario_gui_key_event)` is exactly 8 bytes. `read()` with a
zero-length buffer returns zero; a nonzero buffer smaller than one record or
not an exact record multiple returns `-EINVAL`. A successful read returns as
many queued records as fit, but at least one; nonblocking empty reads return
`-EAGAIN`; `poll()` reports `POLLIN` while records are queued. `/dev/gui`
permits one `O_RDONLY` opener; a concurrent open returns
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
- Synchronize packet framing on the mandatory first-byte bit 3. Discard bytes
  until that bit appears, discard packets reporting PS/2 X/Y overflow rather
  than applying wrapped deltas, and mark the next queued event with the normal
  overflow flag so userspace clears button/grab state. Controller command/ACK
  waits have bounded timeouts; a failed auxiliary-port probe leaves the desktop
  keyboard-only instead of delaying boot.
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

`/dev/psaux` uses the same shared UAPI header and read-count rules. Its event
format is:

```c
struct mario_psaux_event {
    int16_t dx;
    int16_t dy;
    int8_t  dz;      /* always 0 in the first ABI */
    uint8_t buttons; /* bit 0 left, bit 1 right, bit 2 middle */
    uint8_t flags;
    uint8_t reserved;
    uint32_t ticks;  /* monotonic kernel tick captured at IRQ time */
};
```

`sizeof(struct mario_psaux_event)` is exactly 12 bytes. The ABI reserves the
final byte and all unknown flag bits as zero; userspace rejects a future value
until it explicitly supports that version.

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
`poll`, and one-FD `SCM_RIGHTS` transport. The M3 window-manager protocol uses
that implementation directly; do not create a GUI-private socket layer.
Datagrams and abstract names remain out of scope. The first desktop is
multi-process from its first visible frame, so no later transport migration is
required.

The direct i386 `SYS_recvfrom` entry and legacy `socketcall` `recv`/`recvfrom`
operations share the AF_UNIX stream receive implementation. They support ordinary reads,
`MSG_PEEK`, and per-call `MSG_DONTWAIT`; other receive flags, including
`MSG_WAITALL`, return `EOPNOTSUPP` without consuming queued data. Peeking retains
both stream bytes and any attached descriptor. A requested source address is
the peer's pathname, truncated to the supplied capacity while reporting its
full length; an unnamed peer reports zero length. Validate receive buffers,
address outputs, and the complete operation-specific socketcall argument array
before consuming data. `tests/shell/socket-recvfrom.test` exercises raw direct
and legacy syscalls as well as libc wrappers, so an `ENOSYS` fallback cannot
hide an absent direct syscall.

GUI protocol message families:

- `WINDOW_CREATE`/`WINDOW_DESTROY`;
- `WINDOW_CLOSE_REQUEST` for a cooperative server-to-client close request;
- `DESKTOP_CREATE` for the one shell-owned, non-decorated bottom surface;
- `WINDOW_MOVE`/`WINDOW_RESIZE`;
- `WINDOW_CONFIGURE`;
- `WINDOW_SET_TITLE`;
- `SURFACE_BEGIN`/`SURFACE_DATA`/`SURFACE_COMMIT` for one atomic dirty
  rectangle transaction;
- `INPUT_EVENT` and `INPUT_RESET`;
- `WINDOW_FOCUS`/`WINDOW_RAISE`;
- `WINDOW_LIST_SNAPSHOT`/`WINDOW_LIST_ADD`/`WINDOW_LIST_CHANGE`/
  `WINDOW_LIST_REMOVE` and `SHELL_ACTIVATE_WINDOW` for the shell taskbar;
- `LAUNCH_REQUEST`/`LAUNCH_RESULT` for approved application launches;
- `APP_READY`/`APP_EXIT`.

The server owns the framebuffer and input devices. Clients never write to
`/dev/fb0` directly.

Socket and message details:

- The M3 server creates `/tmp/.mario-wm` itself with mode `0700`, validates
  that it is a root-owned directory, and listens on
  `/tmp/.mario-wm/socket` with mode `0600`. At startup it removes only a stale
  socket owned by root; any other existing path is a fatal startup error.
- The first desktop is explicitly single-UID: init, `mario-wm`, and its
  clients run with init's uid, so the `0700`/`0600` endpoint is intentional.
  If Mario later runs GUI clients under another uid, that is a separate
  credential-and-seat design with an authenticated connection policy and tests;
  it must not be enabled merely by relaxing the socket mode bits.
- After binding the public pathname listener, `mario-wm` creates one ordinary
  `AF_UNIX/SOCK_STREAM` `socketpair()` and forks `/usr/bin/mario-desktop` with
  one endpoint duplicated to fd 3 and preserved across `exec` as
  `mario-desktop --wm-fd=3`. That connected endpoint is marked as the shell
  capability before any bytes are read. It is not reachable through
  `/tmp/.mario-wm/socket`, is not selected by a `HELLO` field, and carries no
  raw-device descriptor. The child closes every other inherited descriptor;
  all later application clients connect through the public listener.
- A shell becomes ready after its accepted `HELLO` and the first full current-
  serial commit for its `DESKTOP_CREATE` surface. This is valid while text is
  foreground and is distinct from `/tmp/mario-wm.ready`, which the server
  creates only after an F8 foreground framebuffer flush.
- Admit at most eight ordinary public clients in addition to the one private
  shell endpoint. Reject additional public connects with `ECONNREFUSED` until
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
- Each endpoint owns one FIFO send queue and serializes complete headers and
  payloads onto the stream. A short `send()` retains the unsent suffix and
  resumes only after `POLLOUT`; no second writer may interleave bytes into that
  stream. Server-to-client queues use the output-byte limit: coalesce pending
  pointer-motion events, preserve key/button/reset events in order, and close a
  client that cannot drain the bounded queue without dropping a state-changing
  event.
- Before implementing clients, add `include/gui/protocol.h` with a protocol
  version constant, a complete message-type enum, and packed payload structs.
  A new stream begins with `HELLO { uint16_t major, uint16_t minor; }`; the
  server replies with its accepted version or closes an incompatible client
  with `EPROTONOSUPPORT`; the endpoint's server-assigned role is not present
  in `HELLO`. `WINDOW_CREATE` carries a client request ID,
  dimensions, and a validated UTF-8 title no longer than 256 bytes; its reply
  returns that request ID plus the server-assigned window ID, accepted
  dimensions, and initial `configure_serial` 1. Every later client message
  begins with that window ID. The server
  rejects an unknown window ID or one owned by another client with `EPROTO` and
  disconnects the sender. Any message before `HELLO`, a second `HELLO`, or a
  serial that is zero or not newer than the last committed serial for its window
  is likewise a protocol error and closes that client.
  M9 allows multiple `WINDOW_CREATE` requests on one ordinary connection,
  correlated by request IDs; the existing window-addressed payloads are
  unchanged. Track ownership, configure/frame serials, committed/staging
  pixels, focus, and grabs per window. Destroying one window leaves its
  siblings and endpoint live; endpoint disconnect releases them all. A stale
  or foreign ID must never resolve to a newly allocated record after storage
  reuse; reject ID-space exhaustion rather than wrapping onto a live ID.
- Only the shell-capability endpoint may issue one `DESKTOP_CREATE`. Its reply
  returns a server-assigned window ID and current scanout-sized configure
  serial; it receives the same bounded `SURFACE_BEGIN`/`SURFACE_DATA`/
  `SURFACE_COMMIT` transactions as an ordinary window, but has no decorations,
  always occupies the bottom layer, cannot be moved/resized/closed by normal
  window interaction, and is never reported as an application window. A public
  endpoint that sends `DESKTOP_CREATE`, `SHELL_ACTIVATE_WINDOW`, or a
  window-list-control message receives `EPERM` with no state change.
- After its accepted `HELLO`, the shell receives one ordered
  `WINDOW_LIST_SNAPSHOT`, then `WINDOW_LIST_ADD`, `WINDOW_LIST_CHANGE`, and
  `WINDOW_LIST_REMOVE` notifications for each ordinary window. Each record
  includes the server window ID, title, rectangle, focus state, and
  stable ordering needed for taskbar buttons; it never includes client fds or
  surface pixels. The shell asks the server to activate/raise a taskbar target
  with `SHELL_ACTIVATE_WINDOW`; the server validates that the target still
  exists. Any accepted same-UID client may issue a typed `LAUNCH_REQUEST`; it
  is not a shell capability. The request identifies one approved initial
  application and its bounded argument form; the server, not a client,
  forks/execs and reaps it. It replies with `LAUNCH_RESULT { request_id,
  status }`: `status` is zero only after a child is started, otherwise the
  negative launch error. A later child exec or connection failure is reported
  with the existing `APP_EXIT` status notification.
- `LAUNCH_REQUEST` has a fixed enum rather than a command string:
  `LAUNCH_TERMINAL` carries no argument; `LAUNCH_FILE_MANAGER` and
  `LAUNCH_IMAGE_VIEWER` carry one absolute UTF-8 path of at most 1024 bytes;
  `LAUNCH_OPEN_TEXT` and `LAUNCH_EXECUTABLE` carry one absolute UTF-8 path of
  at most 1024 bytes for a terminal client to handle. Embedded NUL, relative,
  or overlong paths are rejected. The server returns `ENOSYS` until the
  corresponding M5, M6, or M7 client binary is installed. It passes no
  framebuffer, input, listener, or client socket fd to the child; the child
  connects normally through the public pathname listener.
- A title-bar close hit or Super-W makes the server send one ordered
  `WINDOW_CLOSE_REQUEST { window_id }` to the focused window's owner after any
  preceding input event. It is cooperative: the client may discard state and
  send `WINDOW_DESTROY`, retain the window while it asks the user to resolve
  unsaved work, or ignore the request. M3 has no forced-close timeout and the
  server never kills/disconnects a healthy client merely because it retains its
  window. The server destroys all windows owned by a disconnected ordinary
  client. On shell disconnect it destroys the shell's normal windows but keeps
  only the last committed `DESKTOP_CREATE` surface as an input-inert snapshot
  until one replacement shell commits a new desktop surface or the server ends
  the session; it then frees the retained snapshot.
- Window geometry is server-owned. A move changes compositor position only and
  never invalidates a surface. When a user resize or server-side constraint
  changes content dimensions, the server sends
  `WINDOW_CONFIGURE { window_id, uint64_t configure_serial, width, height }`.
  A client tags every `SURFACE_BEGIN` with the current `configure_serial`; an
  older or unknown serial is rejected. A new configure serial cancels any
  staging transaction for that window. Until a full-surface
  `0,0,width,height` transaction for the current serial commits, the compositor
  keeps the prior committed surface clipped and padded to the new geometry
  rather than exposing uninitialized pixels. A newer configure serial
  supersedes the prior one without waiting for a client acknowledgement.
  A newly created window has no prior surface, so the server shows only its
  opaque theme background until the first required full-surface commit.
  From M9, reserve the required capacity before accepting a size or fullscreen
  transition. Reject an over-budget transition without changing mode or
  geometry. If allocation or commit fails after acceptance, restore the prior
  mode and geometry; a corrective configure receives a fresh serial rather
  than rolling the serial back. Release canceled reservations just as staging
  bitmaps are released. This uses the existing configure/error messages, not
  a second rendering protocol.
- `SURFACE_BEGIN` carries `window_id`, the current nonzero little-endian
  `uint64_t configure_serial`, a nonzero `uint64_t frame_serial`, and one
  `x`, `y`, `width`, `height` rectangle. It reserves a staging bitmap for
  exactly that rectangle. `SURFACE_DATA` carries the same window/configure/
  frame serial tuple plus `row_offset` and `rows`; its remaining bytes are
  exactly `width * rows * 4` premultiplied `ARGB8888` pixels. Chunks are
  strictly increasing, contiguous, and fully cover the declared rows.
  `SURFACE_COMMIT` carries the window/configure/frame serial tuple only. All
  wire payloads use fixed-width little-endian fields; no raw C structs
  containing pointers, `size_t`, or `gui_event` are sent. RLE is deferred until
  profiling shows socket copies dominate.
- Validate every wire equation with checked 64-bit arithmetic before allocating
  or copying. In particular, a data message has
  `length == sizeof(header) + 28 + width * rows * 4`; `row_offset + rows` must
  not overflow or exceed the declared height. Reject zero/overflowing
  dimensions, rectangles outside the target surface, and messages that exceed
  the received-byte quota. Never trust a client-sized multiplication performed
  in 32-bit arithmetic.
- Each window has at most one in-flight transaction. The server writes data
  only to its staging bitmap and copies it into the live window surface after
  the matching commit. A newer serial first discards the older staging bitmap;
  a disconnect does the same. The compositor reads only committed live pixels,
  so a partial update can never corrupt the previously visible frame.
- A transaction expires one second after `SURFACE_BEGIN` unless its complete
  ordered data and matching commit arrive first. Expiry frees the staging
  bitmap, preserves the prior live pixels, and reports a recoverable protocol
  error to the client. This prevents an idle but connected client from holding
  the global staging budget indefinitely.
- `WINDOW_DESTROY` immediately cancels the target window's staging transaction,
  frees its live surface, ends any focus/grab state through `INPUT_RESET`, and
  removes queued server-to-client events for that window. Later surface or
  input messages naming the destroyed ID are protocol errors.
- `INPUT_EVENT` is server-to-client only. It carries translated Mario keysyms,
  modifiers, and ticks rather than raw PS/2 scancodes. Pointer coordinates are
  signed content-local coordinates relative to the target window; a held
  client-side grab may therefore receive values outside its content rectangle.
  The server keeps desktop-global pointer coordinates and owns all window
  move/resize grabs. `INPUT_RESET` has no key/button payload and invalidates
  all client input state as defined by the event-loop contract. With multiple
  windows per connection in M9, the existing connection-scoped reset applies
  to every window on that endpoint; `INPUT_EVENT` and focus notifications
  continue to identify their individual target window.
- Bound untrusted client state before the first client is accepted. The initial
  M3-M8 implementation admits eight connections and one window per ordinary
  connection. M9 dynamically allocates connection/window records and permits
  multiple windows per ordinary endpoint, with no small fixed count ceiling.
  Account for the private shell, diagnostic clients, pending launches, poll
  tables, and window records under the metadata byte budgets in the
  [toolkit contract](ui-toolkit.md#surface-memory-and-failure-handling), in
  addition to kernel fd/process limits. No live surface exceeds one
  `gui_frame_bytes` in ARGB pixels. Charge live surfaces, staging bitmaps, and
  outstanding reservations against the peer's aggregate budget without
  double-counting reserved storage when allocated: initially two
  `gui_frame_bytes`, replaced at M9's multiwindow step by
  `min(8 * gui_frame_bytes, 32 MiB)`. The global pool also includes retained
  desktop snapshots: initially 2.5 `gui_frame_bytes`, replaced in M9 by
  `min(16 * gui_frame_bytes, 64 MiB)` with shell replacement headroom as defined
  in the [toolkit contract](ui-toolkit.md#surface-memory-and-failure-handling).
  Limit incomplete input and queued output independently to 64 KiB per client;
  stream large shell snapshots in bounded batches while preserving the ordered
  snapshot/delta sequence. Grow client/window/launch records only after
  resource admission; allocation or quota failure rejects the new operation
  or connection without destroying healthy existing windows.
  The server refuses a request that exceeds an allocation
  limit, coalesces pending mouse-move notifications, and disconnects a client
  that exceeds either byte quota; it never lets a blocked client grow an
  unbounded queue.
- GUI messages do not use `SCM_RIGHTS` in the first protocol version.
  Clients never receive `/dev/fb0`; a later file-picker protocol may pass
  ordinary file descriptors only after explicit lifetime tests.
- On disconnect, the server releases all window surfaces, input grabs, and
  file descriptors associated with that client.
- Protocol tests use `socketpair()` to cover split headers, oversized lengths,
  arithmetic-overflow rectangles, allocation and output-quota exhaustion,
  duplicate/gapped rows, stale serials, disconnects in a staged update, and
  preserving the last committed pixels before pathname-socket tests are
  enabled.
