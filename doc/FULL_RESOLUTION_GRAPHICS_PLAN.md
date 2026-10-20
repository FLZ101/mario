# Full Resolution Graphics Plan

## Goal

Move Mario from the current fixed VGA text console (80x25 at `0xB8000`) to a
linear framebuffer mode using QEMU's VGA device. The primary validation mode is
1920x1080x32, provided by the Multiboot loader through VBE information. The
console should render text at the actual framebuffer resolution, and userspace
should be able to access the framebuffer through `/dev/fb0`.

Measurable success criteria:

- `screendump` from the QEMU monitor shows a pixel framebuffer at the requested
  mode, not the default VGA text plane only.
- `/dev/fb0` reports the expected resolution, stride, and depth through `ioctl`.
- A test can write a known pixel pattern to `/dev/fb0` and compare the QEMU
  screen dump against a golden PPM.
- The console still passes the existing interactive tests (`vim`, `top`,
  `hexedit`) after golden images are updated for the new console geometry.
- Booting without VBE information continues to use the existing 80x25 text
  console as a fallback.

## Behavioral Invariant

This feature is purely additive:

- `/dev/tty1`-`/dev/tty6` keep their existing text-console behavior.
- The serial consoles keep their existing behavior.
- The kernel text console path is not replaced.
- A graphics desktop is added as another switchable console.
- Switching to graphics does not destroy the text VTs; switching back to F1-F6
  or serial restores the same consoles.

## Current Limitations

`src/fs/char/console.c` is a text-mode-only terminal emulator:

- `N_COL` and `N_ROW` are compile-time constants 80 and 25.
- Every console keeps a fixed `uint16_t mem[N_ROW][N_COL]` cell buffer.
- Foreground output writes directly to VGA text memory:

  ```c
  #define VIDEO_MEM ((uint16_t (*)[N_COL])(0xb8000 + KERNEL_BASE))
  ```

- Cursor movement is done with VGA CRT controller ports `0x3D4`/`0x3D5`.
- `console_init()` reports `winsize` as 80x25.

`src/kernel/start.S` requests Multiboot flags `0x00010003`, so it asks for page
alignment, memory information, and address information, but not video mode.
`src/include/multiboot.h` already declares the Multiboot VBE fields, but there
is no VBE mode-info struct and no kernel code that consumes them.

`src/mm/paging.c` maps only RAM into the kernel half. A PCI linear framebuffer
is MMIO outside the E820 RAM map, so it also needs an explicit kernel virtual
mapping.

## Scope and Assumptions

- Initial display device is QEMU `-vga std`.
- Primary target mode is 1920x1080x32 linear graphics.
- If the bootloader cannot provide 1920x1080x32, fall back to the next
  available graphics mode such as 1024x768x32, then to text mode.
- The console geometry is computed from the actual VBE resolution, not
  hard-coded, so modes larger than 1920x1080 can work if the VBE backend and
  compile-time console limits allow it.
- No 2D/3D acceleration, no GPU driver, no display hotplug.
- Text fallback remains mandatory for the custom `boot.bin` path until VBE is
  implemented there.
- The first version may expose `/dev/fb0` through `read()`/`write()`/`ioctl()`.
  Mmap support is a later milestone because the current fault path does not yet
  support physical-device mappings.

## Startup Flow and Desktop

Boot stays in console mode first, then transitions to graphics mode once the
framebuffer and userspace are ready.

### Stage 1: Console-mode boot

1. The bootloader and early kernel use the existing text console path.
2. Kernel `printk` messages go to the text console and serial console.
3. `fb_early_init()` records VBE information; `fb_map()` maps the linear
   framebuffer but does not yet redirect console output.

If no VBE framebuffer is available, boot remains in the existing text-console
mode and starts the normal `getty` processes.

### Stage 2: Enter graphics mode

1. The kernel maps the framebuffer and initializes `/dev/fb0`, but the boot
   foreground console remains a normal text console.
2. A new graphics console is registered alongside `/dev/tty1`-`/dev/tty6`.
3. Pressing F8 activates the graphics console; the desktop then clears the
   framebuffer and takes over rendering.

Existing text virtual consoles remain exactly as they are:

- F1-F6: existing `/dev/tty1`-`/dev/tty6` text terminals
- F8: new graphics console (`/dev/fb0` + `/dev/tty7` + keyboard)
- serial: existing serial shell

The exact key mapping can be adjusted later; the important invariant is that a
text fallback still exists.

### Stage 3: Simple desktop

Use LVGL as the desktop widget toolkit. LVGL is C, MIT-licensed, and designed
for custom framebuffer and input backends, so it fits the pixel-primitive
approach better than FLTK or an X11/Wayland desktop.

Add a small userspace desktop process around LVGL, for example:

- `app/usr/desktop/main.c`
- `app/usr/desktop/lv_conf.h`
- `app/usr/desktop/lv_port_disp.c`
- `app/usr/desktop/lv_port_indev.c`
- `app/usr/desktop/terminal.c`

Desktop responsibilities:

- Open `/dev/fb0`.
- Query mode information with `FBIOGET_VSCREENINFO`/`FBIOGET_FSCREENINFO`.
- Initialize LVGL with a display driver that writes pixels into the framebuffer.
- Open the graphics console tty, for example `/dev/tty7`, in raw mode for
  keyboard input, and feed bytes into an LVGL input device driver.
- Provide a periodic LVGL tick from the existing timer API.
- Build the desktop from LVGL screens and objects instead of hand-rolled
  widgets.

The first desktop should contain at least:

- An LVGL screen with a full-screen wallpaper or solid background.
- A simple panel or taskbar built from LVGL widgets.
- One visible terminal window.
- A blinking or solid text cursor.

LVGL porting surface:

- `lv_port_disp.c`: implement `flush_cb` so a dirty area is copied or mapped to
  `/dev/fb0`. Start with full-screen flushes; optimize with partial flushes
  later.
- `lv_port_indev.c`: translate raw `/dev/tty7` bytes into LVGL key events.
- `lv_conf.h`: disable unused LVGL modules such as filesystem, animations,
  GPU, and image decoders that depend on missing libraries.

The desktop remains a normal Mario userspace program. It does not require
threads, dynamic linking, X11, or Wayland.

### Stage 4: Terminal inside the desktop

The first terminal can avoid the missing PTY support by using pipes:

1. Create two pipes for the shell's stdin/stdout/stderr.
2. Fork a child.
3. In the child, `dup2()` the pipes onto fd 0/1/2 and `execve("/bin/oksh")`.
4. In the desktop, write keyboard bytes to the shell's stdin pipe.
5. Parse shell stdout as a small VT100/ANSI stream.
6. Render text, scrolling, cursor movement, and basic colors into an LVGL
   terminal surface or text-area object.

Limitations of the pipe-based terminal:

- The shell does not see a real tty, so job control and `isatty()` are not
  available.
- Window-size changes require a userspace protocol instead of `TIOCSWINSZ`.

These are acceptable for the first desktop. A proper `/dev/ptmx` and
`/dev/pts/N` implementation can be a later milestone.

### Kernel support required for the desktop

- `/dev/fb0` with read/write/lseek/ioctl as described above.
- A way to prevent the kernel console from drawing over the desktop on the
  foreground framebuffer console.
- A graphics/text mode switch, modeled after Linux `KD_GRAPHICS`/`KD_TEXT`, so
  switching to F8 gives the desktop exclusive framebuffer ownership and
  switching back to F1-F6 restores kernel text rendering.
- A way for the desktop to receive keyboard input while it owns the console;
  the existing `/dev/tty7` raw-mode path is sufficient for the first version.
- Optional `FBIO_WAITFORVSYNC` or a simple timer so the desktop can throttle
  redraws. Do not implement vsync in the first version.

`app/example/init/main.c` should start the desktop on the new graphics console
when `/dev/fb0` is present, while continuing to start normal text `getty`
processes on `/dev/tty1`-`/dev/tty6` and the serial ports.

## Design

### 1. Request a Multiboot video mode

In `src/include/multiboot.h`:

- Add `MB_FLAG_VIDEO` as an alias for the existing `MB_FLAG_VBE`.
- Add the Multiboot video mode type constants:

  ```c
  #define MB_VIDEO_TYPE_GRAPHICS 0
  #define MB_VIDEO_TYPE_TEXT     1
  ```

- Add a packed VBE Mode Info Block structure with at least:

  ```c
  uint16_t mode_attributes;
  uint16_t bytes_per_scanline;
  uint16_t x_resolution;
  uint16_t y_resolution;
  uint8_t  bits_per_pixel;
  uint8_t  memory_model;
  uint32_t phys_base_ptr;
  ```

  The real VBE structure is 256 bytes; the packed struct must preserve the
  physical-layout offsets used by the bootloader.

In `src/kernel/start.S`:

- Change `MB_FLAGS` to `0x00010007`.
- Append four Multiboot header words after the existing address fields:
  mode type `0`, width `1920`, height `1080`, depth `32`.

The custom `src/boot/boot.asm` loader reads only the fields it already
understands and will ignore these extra header words. QEMU's built-in
Multiboot loader, used by `qemu/Makefile` quick targets and
`tests/lit_vga_test.py`, can supply VBE information from this header.

### 1b. Detect and select the highest supported VBE mode

The Multiboot header above expresses only a preferred mode. To choose the
highest mode advertised by the VGA BIOS, the bootloader must enumerate VBE
modes before entering protected mode.

Algorithm for BIOS/VBE 2.0+:

1. Call VBE Function `00h` (`Return VBE Controller Information`).
2. Verify the returned signature is `"VESA"` and the VBE version is at least
   2.0.
3. Read the protected-mode `VideoModePtr` from the controller information.
4. Walk the mode list until the terminator `0xFFFF`.
5. For each mode number, call VBE Function `01h` (`Return VBE Mode
   Information`).
6. Keep only modes that satisfy all of:
   - mode is supported (`ModeAttributes` bit 0),
   - linear framebuffer is available (`ModeAttributes` bit 7) or the mode is
     explicitly banked,
   - `MemoryModel` is packed pixel (`4`) or direct color (`6`),
   - `BitsPerPixel` is 15, 16, 24, or 32,
   - `XResolution`, `YResolution`, `BytesPerScanLine`, and `PhysBasePtr` are
     nonzero.
7. Score each candidate:
   - primary score: `XResolution * YResolution`, descending,
   - secondary score: prefer 32 bpp, then 24, 16, 15,
   - tertiary score: prefer linear framebuffer over banked.
8. Choose the first candidate in that order.
9. Set the mode with VBE Function `02h` (`Set VBE Mode`):
   - OR the mode number with `0x4000` for a linear framebuffer.
   - OR with `0x8000` to keep the existing display contents if that behavior is
     useful during boot.
10. Copy the selected `ModeInfoBlock`, mode number, and controller information
    into the Multiboot information structure, and set `MB_FLAG_VBE`.

The kernel should not trust a hard-coded resolution. `fb_early_init()` must use
the values from the selected `ModeInfoBlock` (`x_resolution`, `y_resolution`,
`bytes_per_scanline`, `bits_per_pixel`, `phys_base_ptr`). If the bootloader
only provided the preferred-mode path, validate that the returned mode is
usable and fall back when it is not.

For `qemu -kernel` with the built-in Multiboot loader, the preferred mode path
is sufficient for testing. For "always highest supported by the hardware", the
real-mode enumeration belongs in `src/boot/boot.asm` or in GRUB's `vbeprobe`
logic when booting through GRUB.

This plan uses "highest supported by the VBE BIOS", not EDID/DDC. Detecting the
monitor's physical preferred resolution requires a VBE/DDC implementation over
the display data channel and is out of scope.

### 2. Capture and map the framebuffer

Add a small video subsystem, for example:

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

1. Call `fb_early_init(struct multiboot_info *m)` before
   `paging_init()` in `src/kernel/mario.c`. This copies the VBE fields while
   the low Multiboot memory is still mapped.
2. Call `fb_map()` after `paging_init()`. It creates explicit kernel page-table
   entries for the framebuffer physical range.
3. Use a reserved kernel virtual window such as `0xF0000000`
   (`KERNEL_BASE + 0x70000000`) for the linear framebuffer. With the current
   256 MiB QEMU memory size this does not overlap RAM mappings. The window size
   and conflict check should be explicit rather than hard-coded silently.
4. If Multiboot `MB_FLAG_VBE` is absent, leave `fb_info.enabled == 0` and keep
   the existing text console.

The early-init/copy split is required because `paging_init()` unmaps the low
16 MiB after `ramdisk_setup()`; the VBE mode info lives in bootloader-provided
low memory.

### 3. Raw framebuffer device `/dev/fb0`

Add a character device:

- `src/fs/char/fb.c`
- `FB_MAJOR` and `FB_MINOR` in `src/include/fs/chrdev.h`
- `fb0 C 29 0` in `rd/device_list`

Raise `MAX_CHRDEV` from 16 to 32 so Linux framebuffer major 29 is accepted.

Initial file operations:

- `open`: select `/dev/fb0` and mark the file position initially zero.
- `lseek`: allow positions `0..fb.size`, because device inodes do not carry the
  framebuffer size through the default inode-size path.
- `read`: copy `count` bytes from `fb.virt_base + f->f_pos` to the user buffer.
- `write`: copy `count` bytes from the user buffer to the framebuffer.
- `release`: no-op initially.
- `ioctl`: implement the minimal Linux framebuffer ABI:
  - `FBIOGET_VSCREENINFO`
  - `FBIOGET_FSCREENINFO`
  - `FBIOGET_VBLANK` can return unsupported or zeroed data.
  - Mode changes (`FBIOPUT_VSCREENINFO`) are deferred; return `-EINVAL`.
- `mmap`: deferred until physical-device VMA mapping is supported.

Copying must use the existing uaccess helpers:

```c
verify_area(VERIFY_WRITE, user_buf, count);
memcpy_tofs(user_buf, fb.virt_base + pos, count);
```

### 4. Graphics console integration

The existing kernel text console remains unchanged. `src/fs/char/console.c`
only needs to recognize one additional graphics console slot; it does not need
to become a full framebuffer text renderer for the first implementation.

The userspace desktop is responsible for rendering into `/dev/fb0`. If a
kernel-rendered full-resolution text console is desired later, the following
refactor would make cell drawing backend-independent.

Replace direct `VIDEO_MEM` writes with helpers:

```c
console_paint_cell(struct console *con, int row, int col, uint16_t cell);
console_paint_rect(struct console *con, int row, int col, int rows, int cols);
console_paint_cursor(struct console *con);
```

Text mode keeps those helpers writing to `0xB8000`. Framebuffer mode renders a
glyph through an embedded 8x16 CP437 font and uses the VGA attribute byte for
foreground/background color translation.

Geometry:

- `cols = fb.width / 8`
- `rows = fb.height / 16`
- Clamp to compile-time maximums:

  ```c
  #define CONSOLE_MAX_COLS 320
  #define CONSOLE_MAX_ROWS  96
  ```

  For 1920x1080x32 with an 8x16 font this is 240x67. The maximums leave
  headroom for slightly larger modes. Fixed maximums avoid heap-backed console
  buffers before the page allocator is available.
- Report the new geometry in `tty->winsize`.

Add a console type to `struct console`:

```c
enum console_type {
    CONSOLE_TYPE_TEXT,
    CONSOLE_TYPE_GRAPHICS,
};
```

- `/dev/tty1` through `/dev/tty6` remain `CONSOLE_TYPE_TEXT`.
- The new graphics console, initially `/dev/tty7`, is
  `CONSOLE_TYPE_GRAPHICS`.
- Text consoles continue to own their cell buffers. The graphics console is a
  userspace-owned surface and the kernel never paints text cells into it.

Refactor these operations to use the paint helpers:

- `write_char`
- `erase_screen` and `erase_line`
- `__scroll`
- `delete_one_char` and `insert_one_char`
- `clear_screen`, `reset_screen`, and alt-screen switching
- `switch_fg_console`

`console_init()` should:

1. Keep the existing text console initialization path.
2. Register the graphics console as the last tty device when `fb.enabled`.
3. Keep the boot foreground on `/dev/tty1`; do not switch to graphics until
   F8 is pressed.
4. Leave text console geometry and rendering behavior unchanged.

`switch_fg_console()` behavior:

- Switching to F1-F6 copies the selected text buffer to the framebuffer.
- Switching to F8 marks the graphics console active and notifies the desktop
  process instead of copying text cells.
- Switching away from F8 tells the desktop it no longer owns the framebuffer;
  the kernel then redraws the selected text console.

Keep the existing 80x25 logic as the fallback path. This keeps
`printk_init()` and early boot output safe before the framebuffer is mapped.

### 5. Cursor handling

In text mode, continue using the VGA CRT controller. In framebuffer mode:

- Disable the hardware VGA cursor.
- Draw an underline or block cursor at `(pos_x * 8, pos_y * 16)`.
- Redraw the cursor cell on movement.
- Honor `cursor_hidden` and terminal escape sequences such as DECTCEM.

### 6. QEMU/test integration

`qemu/Makefile`:

- Add `-vga std` to `QUICK_QEMU_FLAGS`.
- Keep `quick-run-gtk` for visible framebuffer work.

`tests/lit_vga_test.py`:

- Add `-vga std` to the QEMU command.
- Add a test that opens `/dev/fb0`, writes a pixel rectangle, uses
  `VGA-DUMP-SCREEN`, and compares the result to a golden PPM.
- Existing text assertions currently read `0xB8000` directly. Preserve a
  25x80 text shadow or update the test harness to read the console cell buffer
  through debugfs. Prefer the compatibility shadow during the transition.

Golden VGA PPM files in `tests/vga/*.ppm` need regeneration once the console
geometry changes.

## Files to Change

Boot and Multiboot:

- `src/kernel/start.S`
- `src/include/multiboot.h`
- `src/boot/boot.asm` only if the custom bootloader is later upgraded to VBE

Core framebuffer:

- `src/include/video/fb.h`
- `src/video/fb.c`
- `src/kernel/mario.c`
- `src/mm/paging.c`

Console:

- `src/include/fs/tty/console.h`
- `src/fs/char/console.c`

Device:

- `src/include/fs/chrdev.h`
- `src/fs/char/fb.c`
- `src/fs/char/chrdev.c`
- `rd/device_list`
- `doc/devices.txt`

Build/test:

- `src/Makefile` if `src/video` is a new directory
- `qemu/Makefile`
- `tests/lit_vga_test.py`
- `tests/vga/fb0.test` and a golden PPM

Desktop:

- `app/example/init/main.c`
- `app/usr/lvgl/` vendored LVGL source tree
- `app/usr/desktop/main.c`
- `app/usr/desktop/lv_conf.h`
- `app/usr/desktop/lv_port_disp.c`
- `app/usr/desktop/lv_port_indev.c`
- `app/usr/desktop/terminal.c`
- `app/usr/Makefile` to build and install the desktop

## Implementation Order

### Milestone 1: Boot framebuffer detection and kernel mapping

1. Add Multiboot video header fields and mode-info structure.
2. Add `fb_early_init()`/`fb_map()` and call them from `mario()`.
3. Add temporary `printk` diagnostics for width, height, bpp, stride, and
   physical base.
4. Validate with `make quick-run-gtk`; the diagnostic should report
   1920x1080x32 when booted with QEMU's Multiboot loader.

### Milestone 2: `/dev/fb0` with read/write/lseek/ioctl

1. Register the framebuffer character device.
2. Add `fb0` to the ramdisk device list.
3. Add a small test program or shell test that writes a color pattern and
   reads it back.
4. Add a VGA test that uses `screendump` to verify visible output.

### Milestone 3: LVGL framebuffer port

1. Vendor an LVGL release under `app/usr/lvgl/`.
2. Create a minimal `lv_conf.h` for framebuffer-only use.
3. Implement the LVGL display driver over `/dev/fb0`.
4. Implement a keyboard input driver over `/dev/tty7`.
5. Add a demo that draws text and widgets, and verify it with `screendump`.

### Milestone 4: Simple desktop and terminal

1. Add the desktop process under `app/usr/desktop/`.
2. Start it on the graphics console when `/dev/fb0` is available.
3. Build the background, taskbar/status line, and one terminal window with
   LVGL widgets.
4. Run `oksh` through pipes and render its output in the terminal window.
5. Add F8 switching to the new graphics console while F1-F6 remain text VTs.
6. Add a VGA test that launches the desktop and checks the initial screen.

### Milestone 5: Performance and userspace mmap

1. Add dirty-region tracking if full-screen redraws are too slow.
2. Add `VM_PFNMAP` support to `src/mm/fault.c` so `/dev/fb0` can be mmapped
   directly instead of requiring read/write copies.
3. Optimize scrolling to use framebuffer line moves.

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

Framebuffer device test:

```sh
.venv/bin/python tests/lit_vga_test.py --test fb0.test tests/vga
```

Console regression:

```sh
.venv/bin/python tests/lit_vga_test.py tests/vga
```

Desktop startup:

```sh
make quick-run-gtk
```

- Confirm boot messages appear first in console mode.
- Confirm boot stays on F1 text console until F8 is pressed.
- Confirm F8 opens the graphics desktop and terminal.
- Confirm F1-F6 still switch to text terminals.

Text fallback:

- Boot through the existing `boot.bin` path without VBE.
- Confirm the console still uses 80x25 VGA text.
- Run `tests/lit_vga_test.py` before the framebuffer-only test is enabled in
  the no-VBE path.

## Risks and Rollback

- Framebuffer physical address may be above the currently mapped RAM window.
  Keep the reserved MMIO window conflict check explicit and fall back to text
  mode if the mapping cannot be created.
- Existing VGA tests depend on `0xB8000` and 25x80 geometry. Preserve a text
  shadow and update the harness incrementally.
- The custom `boot.bin` path does not provide VBE information. Do not remove
  the text path until that loader is upgraded.
- `mremap` is currently intentionally unimplemented, but `/dev/fb0` mmap will
  later need physical-mapping support; this plan does not depend on `mremap`.
