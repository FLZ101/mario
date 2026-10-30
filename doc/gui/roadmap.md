# GUI Implementation Roadmap

Part of the [GUI platform plan](README.md). Follow these milestones in order;
each has a concrete deliverable and a validation gate.


This section is the executable form of the plan. Work through the milestones
in order. A milestone may use several small, reviewable commits, but it is not
complete until its full checkpoint is green. Do not start a later milestone
while an earlier checkpoint is red.

## Guide Conventions

- Files listed under **Change** are the only files expected to change for that
  milestone. If a needed file is missing from the list, add it and update the
  list.
- Always run `git diff --check` before committing.
- Run the named shell, TTY, or GUI test after every kernel/userspace change. A passing
  checkpoint is required before the next milestone.
- Keep every implementation commit narrow and use the suggested subject for
  the milestone's final integration commit unless the user asks otherwise.
- If a milestone check fails, roll back the local change, make the smallest
  possible fix, and re-run only the failing test before resuming.
- Text tests run through the serial/tmux harness or a text VT, never the
  graphics session. GUI tests start from a text console, then use F8 to enter
  graphics mode.
- Mario builds one common image. It always requests VBE, includes the
  framebuffer/input drivers and GUI payload, and uses F1-F6 versus F8 only as
  a runtime foreground-console transition.
- Text and GUI tests run against that same image. `tests/lit_tty_test.py`,
  `tests/lit_shell_test.py`, and the future `tests/lit_gui_test.py` run their
  respective selected cases; GUI tests explicitly enter F8, while text tests
  stay in the terminal harness.
- When a milestone introduces a new GUI binary, update `app/usr/Makefile` and
  `rd/copy_files.py` in that milestone; verify with `rd/make-rd0.py` that
  `make image` installs and strips the binary into the common `ramdisk0`.

## Milestone Map

| Milestone | Deliverable | Exit gate |
| --- | --- | --- |
| M0 — Framebuffer platform | VBE handoff, `/dev/fb0`, and framebuffer text VTs | `fb0.test` and `text-vt.test` |
| M1 — Graphics session and test control | F8 graphics mode, raw input, and GUI-runner primitives | console/input shell and GUI tests |
| M2 — Toolkit and fonts | `libmario-ui`, pinned FreeType, bundled fonts | font and event screenshots |
| M3 — Single-process desktop | compositor, windows, init integration, readiness/frame diagnostics | desktop and window screenshots |
| M4 — PTY prerequisite gate | existing Unix98 PTY/tmux contract | PTY and tmux regressions |
| M5 — GUI terminal | VT emulator, truecolor, PTY shell, tmux panes | terminal screenshot regression |
| M6 — Image viewer | BMP/PNG decode, fit, zoom, pan | image-viewer screenshot regression |
| M7 — File manager | browse, mutate `/tmp`, and launch apps | file-manager end-to-end regression |
| M8 — Multi-process desktop | `mario-wm`, protocol, and client lifecycle | multi-process GUI regression |

Before M0:

```sh
make shell-test
make tty-test
```

Record the baseline result. Any failure here is pre-existing and must be fixed
or consciously excluded before GUI work begins.

## M0: Framebuffer Platform

Goal:

- Boot QEMU with a VBE linear framebuffer.
- Map the framebuffer into kernel virtual memory.
- Create `/dev/fb0` with read, write, lseek, and framebuffer info ioctls.
- Render and switch F1-F6 text VTs before any GUI session or desktop starts.

Change:

- `Makefile`: keep one build/image configuration; do not add a `GUI` build
  flag, a text-only boot policy, or a second image target. The image always
  requests a framebuffer and initializes it when the handoff is valid.
- `Makefile`: add `GUI_TEST_CASES ?= tests/gui` and `gui-test`, which invokes
  `tests/lit_gui_test.py`; add it to `test` after
  `fb0.test` exists. Keep `vga-test` as an intentional no-op compatibility
  target and never attach GUI coverage to it.
- `src/include/multiboot.h`: add `MB_FLAG_VIDEO`, video type constants, and a
  packed VBE mode info structure.
- `src/kernel/start.S`: set `MB_FLAGS` to `0x00010403` (existing flags plus
  `MB_FLAG_VBE`) and append mode type 0, width 1024, height 768, depth 32 in
  the single image. Keep 1920x1080 as an interactive build-time request only
  after the baseline QEMU test is stable.
- `src/include/video/fb.h`: add `struct fb_info`.
- `src/video/fb.c`: add `fb_early_init()` and `fb_map()`.
- `src/kernel/mario.c` or the current kernel entry file: call `fb_early_init()`
  before paging teardown and `fb_map()` after paging is stable. Start the one
  `fbcon` kernel thread only after `sched_init_smp()`; it waits on `fbcon_wait`
  and is the sole framebuffer text-painting context. `fbcon_start()` queues a
  full initial `tty1` repaint before the first userspace process runs.
- `src/include/video/fbcon.h` and `src/video/fbcon.c`: add the kernel-only
  framebuffer text presenter. It consumes existing `struct console` cell
  buffers, uses a fixed CP437 bitmap/palette, honors real stride and masks, and
  implements a software cursor without depending on `/dev/fb0` or userspace.
- `src/video/font_cp437_8x16.inc`: add the checked-in 256-glyph bitmap generated
  once from the bundled DejaVu Sans Mono source, with source/license metadata.
- `src/fs/char/console.c`: render the existing 80x25 text-VT cell buffers with
  a VGA-compatible 9x16 CP437 renderer into a centered `720x400` canvas
  whenever a framebuffer is enabled; retain the legacy-VGA rendering path when
  no framebuffer handoff is present. Replace every direct `VIDEO_MEM` mirror
  with cell/span/row/full presentation calls, mark dirty bounds under
  `console_lock`, and wake the `fbcon` thread after releasing that lock. Normal
  output repaints changed cells only; only clear, alternate-screen, scroll, and
  a foreground-VT switch may repaint broad regions.
- `src/include/fs/chrdev.h`: add `FB_MAJOR 13` and `FB_MINOR 0`.
- `src/fs/char/fb.c`: implement open/lseek/read/write/release/ioctl.
- `rd/device_list`: add `fb0 C 13 0`.
- `tests/gui/fb0.test`: write a known pixel pattern and compare a screendump.
- `tests/gui/text-vt.test`: use serial control to write a distinct fixed marker
  at the same known cell location on each `/dev/tty1` through `/dev/tty6`,
  inject F1-F6 through QMP, and compare that text-canvas crop after every
  switch. It starts no GUI-seat helper and proves the framebuffer text path
  independently of the desktop.
- `tests/lit_gui_test.py`: add a dedicated QMP/serial test runner. It uses QMP
  for `screendump` and input injection, serial only for guest control/status,
  and never shares implementation with the tmux text runner.

Build and run:

```sh
make kernel
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test fb0.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test text-vt.test tests/gui
make gui-test
```

Checkpoint:

- Kernel `printk` reports a nonzero width, height, stride, bpp, and physical
  base.
- `fb0.test` passes.
- `text-vt.test` proves F1-F6 show the correct independent 80x25 cell buffer
  through the framebuffer renderer while no `/dev/gui` owner or desktop process
  exists. The test retries its expected crop rather than sleeping, proving the
  asynchronous `fbcon` paint has completed. F8 remains unavailable in M0.
- The same image passes all shell and TTY tests; without VBE information it
  falls back to legacy text output.
- `fb0.test` performs its raw write/readback before M1 installs foreground-GUI
  write gating; M0 proves the device ABI, not desktop ownership.

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

## M1: Graphics Session, Input, and Test Control

Goal:

- Add a tty-independent graphics-console mode and its `/dev/gui` keyboard
  event endpoint.
- Keep F1-F6 text consoles unchanged and reserve all F1-F12 keys in the
  kernel.
- Enable the PS/2 mouse and expose `/dev/psaux`.

Change:

- `src/include/fs/tty/tty.h` and `src/include/fs/tty/console.h`: add an explicit
  `FOREGROUND_TEXT`/`FOREGROUND_GRAPHICS` state plus `fg_text_console`; retain
  `switch_text_console(index)` for F1-F6 and add `enter_graphics_console()`
  for F8. The graphics console has no tty minor and never consumes a text-VT
  index.
- `src/include/fs/gui.h`: define `struct gui_key_event` plus the
  `GUI_KEY_EVENT_OVERFLOW` flag, 256-record queue, `gui_init()`,
  keyboard-queue, and foreground-state interfaces used by the console and
  keyboard drivers.
- `src/fs/char/gui.c`: register `/dev/gui` at `GUI_MAJOR 11`, `GUI_MINOR 0`;
  implement its exclusive read-only open, fixed-record read, and `poll()`.
- `src/fs/char/tty.c`, `src/fs/char/console.c`, and `src/fs/char/kbd.c`:
  initialize the GUI device before PS/2 input is enabled, reserve F1-F12
  before looking up a tty, route F1-F6 through `switch_text_console()` and F8
  through `enter_graphics_console()`, leave F7/F9-F12 as reserved no-ops, and
  enqueue non-function keys only while graphics mode is foreground. In graphics
  mode, the last text tty must receive no keyboard bytes. Notify the desktop on
  switch-away/switch-back.
- `src/include/video/fb.h` and `src/fs/char/fb.c`: add
  `FBIOGET_CONSOLE_STATE` and wake `/dev/fb0` pollers with `POLLPRI` on
  console switches. M0 permits the focused raw `fb0.test` writer; after M1,
  ordinary `/dev/fb0` writes return `-EBUSY` unless graphics is foreground.
  The kernel `fbcon` presenter bypasses this file-operation gate. User writes
  copy in bounded scanline-sized chunks with a foreground-generation check;
  transition repaint wins over any in-flight stale GUI write.
- `src/include/fs/psaux.h` and `src/fs/char/psaux.c`: enable the auxiliary
  PS/2 port, decode the basic 3-byte packet into the fixed 256-record
  `psaux_event` queue, set `PSAUX_EVENT_OVERFLOW` after a drop, and implement
  its exclusive read/poll interface. `dz` is zero; wheel negotiation is out of
  scope for this milestone.
- `src/include/fs/chrdev.h`: add `GUI_MAJOR 11`, `GUI_MINOR 0`,
  `PSAUX_MAJOR 10`, and `PSAUX_MINOR 1`.
- `rd/device_list`: add `gui C 11 0` and `psaux C 10 1`.
- `app/test/gui-seat/main.c`: add a deliberately small ABI probe that opens
  `/dev/fb0` for exclusive write access, claims `/dev/gui`, reports state over
  the serial shell, and blocks until signalled. It is used only until M3 proves
  the real desktop owner; it does not draw or bypass routing.
- `tests/gui/gui.console.test`: verify F1-F6 and F8 switching, and verify
  F7/F9-F12 neither change the foreground console nor reach `/dev/gui`.
- `tests/shell/mouse.test`: use the QEMU PS/2 mouse fixture to verify `poll()`,
  full `psaux_event` reads, and overflow-state recovery.
- `tests/lit_gui_test.py`: add the GUI readiness, frame-wait, mouse-input, and
  pixel-rectangle directives specified in the Validation section. The runner
  owns QMP input injection and screendumps; test files name only GUI actions.

Build and run:

```sh
make kernel
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.console.test tests/gui
.venv/bin/python tests/lit_shell_test.py tests/shell/mouse.test
```

Checkpoint:

- F1-F6 still display the existing text consoles.
- F8 selects the graphics console only while `/dev/gui` has an owner; F1-F6
  restore the selected text VT and its framebuffer-rendered contents.
- `gui.console.test` verifies F8 first produces the deterministic black
  graphics scanout before any desktop exists, rather than retaining pixels from
  the prior text VT. It also verifies F1-F6 repaint the selected text VT before
  the `/dev/fb0` `POLLPRI` notification is observed.
- `gui.console.test` starts `gui-seat` through serial before injecting F8, then
  verifies that non-function keys do not reach the prior text tty, killing it
  returns immediately to that prior text VT, and a later F8 press is a no-op.
- Reading `/dev/gui` does not echo or apply the normal line discipline; a
  second reader is rejected with `-EBUSY`.
- No F1-F12 key event, modified or unmodified, is readable from `/dev/gui`.
- `/dev/gui` and `/dev/psaux` report overflow explicitly, so the desktop can
  clear stuck key/button state.
- `/dev/psaux` produces relative movement/button events in the QEMU fixture;
  a missing auxiliary device produces `POLLHUP` and leaves the desktop
  keyboard-only.

Debug:

- If F8 changes the console but the desktop area stays black, ownership
  notification is firing but no GUI process is drawing yet; this is expected
  until M3.
- If PS/2 initialization hangs, gate the auxiliary-port probe behind a short
  timeout and leave mouse support disabled on failure.

Suggested commit:

```text
console/input: add graphics mode, /dev/gui, and /dev/psaux
```

## M2: Toolkit and Fonts

Goal:

- Vendor and build FreeType as a static Mario library.
- Implement `gui_surface`, clipping, damage regions, glyph rendering, and a
  CP437 fallback.
- Add a minimal event loop around `poll()`.

Change:

- `app/usr/freetype/`: vendor the pinned source.
- `app/usr/build-freetype.sh` and `app/usr/freetype.mk`: build with
  `util/mario-musl-cc`; pin the archive version, URL, and SHA-256; enable
  SFNT/TrueType/CFF and disable unused modules.
- `app/usr/libmario-ui/include/gui/*.h`: define the core types and public API from
  the design section.
- `app/usr/libmario-ui/src/surface.c`: fill, blit, blend, lines, clipping.
- `app/usr/libmario-ui/src/font.c`: FreeType loading, UTF-8 conversion, glyph cache,
  and CP437 fallback.
- `app/usr/libmario-ui/src/event.c`: keyboard/mouse translation and event queue.
- `app/usr/libmario-ui/src/widget.c`: first `gui_label`/`gui_button` widgets.
- `app/usr/libmario-ui/assets/fonts/`: bundle the pinned DejaVu Sans and
  DejaVu Sans Mono sources; `rd/copy_files.py` installs them under
  `/usr/share/fonts` in the common image.
- `tests/gui/gui.font.test`: render a known string and compare the screen.
- `tests/gui/gui.event.test`: inject keyboard/mouse input and verify a moving
  cursor or button state.

Build and run:

```sh
make app
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.font.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.event.test tests/gui
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
libmario-ui: add surfaces, TTF rendering, and event loop
```

## M3: Single-Process Desktop

Goal:

- Create `mario-desktop` with a solid background, panel, cursor, and overlapping
  windows.
- Implement move/resize/raise/close/focus and damage-region compositing.

Change:

- `app/usr/desktop/main.c`: application entry and console ownership; remove
  stale desktop status files at startup. After an F8-triggered full
  composition, atomically publish the monotonically increasing decimal frame
  number to `/tmp/mario-desktop.frame`, then create
  `/tmp/mario-desktop.ready`. Remove `ready` immediately on switch-away;
  remove all status files on clean exit. Publish `/tmp/mario-desktop.debug`
  with display, focus, pointer, and recent-event state for test failures.
- `app/usr/desktop/demo.c`: `Welcome` and `Display` built-in diagnostic
  windows with the fixed initial geometry from `desktop.md`; they exercise
  normal window-manager paths and are not test-only bypasses.
- `app/usr/desktop/wm.c`: z-order, focus, hit testing, move/resize.
- `app/usr/desktop/compositor.c`: dirty-rectangle collection and `/dev/fb0`
  flush.
- `app/usr/desktop/panel.c`: launcher, window list, and static `--:--` clock.
- `app/usr/desktop/cursor.c`: pointer tracking and cursor sprite.
- `app/usr/desktop/theme.c`: fixed first-milestone geometry, colors, and
  cursor used by screenshot tests.
- `app/usr/Makefile`: add the desktop binary.
- `rd/copy_files.py`: copy `mario-desktop` to `/usr/bin` in the common image.
- `app/system/init/main.c`: after mounting `/usr`, detect the desktop binary,
  spawn it in a new session, and respawn it with the existing three-second
  rate limit after exit.
- `tests/gui/gui.desktop.test`: verify panel, background, cursor, and window
  raise/focus behavior.
- `tests/gui/gui.window.test`: verify window move, resize, stacking, and
  damage-region redraws with exact screenshot crops.

Build and run:

```sh
make app
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.desktop.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.window.test tests/gui
```

Checkpoint:

- F8 shows the desktop; F1 returns to text and F8 back restores the desktop.
- The runner records the current frame number, sends F8, waits for
  `/tmp/mario-desktop.ready`, then waits for a monotonically newer
  `/tmp/mario-desktop.frame` after every injected GUI action. `ready` exists
  only while graphics is foreground and only after its corresponding full
  composition completed. A missing frame file is baseline zero; serial control
  can read all three status files.
- `Welcome` and `Display` begin at their documented locations and exercise
  focus, move, resize, raise, and close without requiring a later application
  milestone.
- A click raises/focuses a window; Alt-Tab cycles focus; Super-W closes the
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

## M4: PTY Prerequisite Gate

Goal:

- Prove that the existing PTY and tmux implementation is suitable for the GUI
  terminal before adding GUI-side VT emulation.
- Make no PTY, tty, or socket kernel ABI changes in this milestone.

Change:

- `doc/PTY_TMUX_PLAN.md`: retain ownership of the PTY/socket ABI and native
  lifecycle tests.
- `tests/tty/tmux-1.test`: retain the real pane/window/split acceptance test
  as the GUI terminal prerequisite.

Build and run:

```sh
make tty-test TTY_TEST_CASES=tests/tty/tmux-1.test JOBS=1
```

Checkpoint:

- tmux creates panes, splits a window, creates a second window, returns to the
  split window, and leaves its server alive.
- The checked tmux panel baseline contains the split borders, pane output, and
  status bar.
- Any PTY regression is fixed in the PTY/tmux work before beginning M5.

## M5: GUI Terminal

Goal:

- Implement the VT emulator.
- Fork `/bin/oksh` on a PTY slave and connect it to a GUI window.

Change:

- `app/usr/terminal/main.c`: open PTY, fork shell, run event loop.
- `app/usr/terminal/vt.c`: cursor, erase, scroll, 16/256/direct-RGB SGR in
  semicolon and colon forms, alternate screen, tabs, and terminal status
  responses.
- `app/usr/terminal/term.c`: terminal grid, scrollback, resize.
- `app/usr/Makefile`: add the terminal binary.
- `app/usr/build-ncurses.sh`: enable ncurses extended and direct RGB colors.
- `rd/copy_files.py`: include the ncurses `xterm-direct` terminfo entry.
- `tests/gui/gui.terminal.test`: open a terminal, run commands, resize it, and
  check the screen.

Build and run:

```sh
make app
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.terminal.test tests/gui
```

Checkpoint:

- `oksh`, `vim`, `top`, `less`, and interactive Python render acceptably.
- Resizing the window changes `stty size` and redraws the grid.
- Ctrl-C interrupts the foreground process.
- `printf '\033[48;2;1;2;3m \033[0m'` and the colon-form equivalent render
  the exact direct-RGB background in the terminal screenshot; `echo $TERM`
  and `echo $COLORTERM` print `xterm-direct` and `truecolor`.
- `tmux new-session`, a split-pane command, and a detached session render and
  exit cleanly inside the GUI terminal.
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

## M6: Image Viewer

Goal:

- Vendor and build libpng as a static Mario library.
- Decode BMP/PNG and implement fit/zoom/pan/next/previous.

Change:

- `app/usr/libpng/`: vendor pinned sources.
- `app/usr/build-libpng.sh`, `app/usr/libpng.mk`.
- `app/usr/libmario-ui/src/image.c`: BMP/PNG decode to `gui_surface`.
- `rd/copy_files.py`: install `tests/gui/assets/` under
  `/usr/share/mario-tests/gui`, so guest tests can copy its
  immutable fixture tree to `/tmp/gui-fixtures/` before every mutation.
- `app/usr/imageviewer/`: viewer application and image cache.
- `app/usr/Makefile`: add the image viewer binary.
- `tests/gui/gui.imageviewer.test`: open a known BMP and PNG, zoom/pan, and
  check the screen.

Build and run:

```sh
make app
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.imageviewer.test tests/gui
```

Checkpoint:

- BMP and PNG open correctly.
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
imageviewer: decode BMP/PNG and add zoom/pan
```

## M7: File Manager

Goal:

- List and navigate directories.
- Open, copy, move, rename, delete, and create directory entries.
- Launch the terminal and image viewer from the file manager.

Change:

- `app/usr/filemanager/`: directory listing, navigation, file operations, and
  launch logic.
- `app/usr/Makefile`: add the file manager binary.
- `tests/gui/gui.filemanager.test`: navigate `/tmp`, create files, rename,
  copy, delete, and launch an image.

Build and run:

```sh
make app
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.filemanager.test tests/gui
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

## M8: Multi-Process Desktop

Goal:

- Make `mario-wm` the GUI server and convert applications into clients.
- Use Mario's existing pathname `AF_UNIX/SOCK_STREAM` transport; do not add a
  second socket implementation.

Change:

- `app/usr/libmario-ui/protocol.c`: bounded length-prefixed message encode/decode,
  including fragmented-read reassembly and `EPROTO` handling.
- `app/usr/libmario-ui/include/gui/protocol.h`: protocol version, complete
  message enum, and packed payload structs matching the IPC contract.
- `app/usr/desktop/`: split `mario-wm` server from `mario-desktop` client.
- `app/usr/terminal/`, `app/usr/imageviewer/`, `app/usr/filemanager/`: move
  window surface updates behind GUI protocol messages.
- `rd/copy_files.py` and `app/system/init/main.c`: install `/usr/bin/mario-wm`
  and make init prefer it over the Phase 1 `/usr/bin/mario-desktop` session
  binary.
- `tests/gui/gui.mp.test`: create a client window, move it, receive input, and
  close it.

Build and run:

```sh
make app
make image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui.mp.test tests/gui
```

Checkpoint:

- Applications run as separate processes.
- Only `mario-wm` opens `/dev/fb0`; clients never receive the framebuffer fd.
- A client disconnect removes its windows and releases the PTY/input resources.
- The single-process Phase 1 build remains available until all Phase 2 apps
  are converted and stable.

Debug:

- Start with `socketpair()` between a parent and child before binding the
  pathname socket; this isolates protocol bugs from server lifecycle bugs.
- If window updates race, make every message length-prefixed and processed to
  completion before the next `poll()` iteration.
- If a client crash leaves a window behind, verify the server closes the
  connection on `recv()` returning zero or error and cleans up client-owned
  surfaces.

Suggested commit:

```text
gui: move window manager and apps to AF_UNIX multi-process model
```

## Final Integration Checklist

Run the complete test suite against the common image:

```sh
make shell-test
make tty-test
make gui-test
```

Run the manual smoke test from the Validation section and confirm all ten
items pass. After that, the GUI plan is complete enough to enter hardening:
measure ramdisk growth, cap font/image caches, and improve the VT emulator
only for the supported apps.

## Validation

Before starting:

```sh
make shell-test
make tty-test
```

After each milestone:

```sh
make kernel
make image
make quick-run-gtk
```

M4 uses its focused tmux gate. After every GUI-facing change, also run the
affected text regression; it boots the same image and stays on a text VT.

Targeted tests:

```sh
.venv/bin/python tests/lit_gui_test.py --test fb0.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.desktop.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.terminal.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.imageviewer.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.filemanager.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui.font.test tests/gui
.venv/bin/python tests/lit_shell_test.py tests/shell/pty.test
```

GUI-runner and input-harness details:

- GUI tests use a fresh QEMU instance with a serial control channel and a QMP
  Unix socket. They start on the initial text console, enter graphics with F8,
  and finish by returning to text. The runner owns QMP's
  `send-key`, `input-send-event`, and `screendump` syntax; test files use the
  stable `GUI-SEND-KEY`, `GUI-MOUSE-MOVE`, `GUI-MOUSE-BUTTON`, and
  `GUI-CHECK-SCREEN-RECT` directives.
- `tests/lit_gui_test.py` is deliberately separate from `tests/lit_tty_test.py`:
  the latter captures host tmux text panels, while the former compares QEMU
  framebuffer screenshots and never depends on host terminal rendering.
- At runner startup, query QMP capabilities and required commands before
  booting a test. A missing `send-key`, `input-send-event`, or `screendump`
  fails the suite with the QEMU version and missing command; it must not skip
  the visual assertion or fall back to host-generated input.
- Add `SERIAL-WAIT-FILE: <guest-path>`, which repeatedly evaluates a shell
  existence check through the serial console. The host must never try to read
  a guest `/tmp` path directly. GUI tests send F8 first, then wait for
  `SERIAL-WAIT-FILE: /tmp/mario-desktop.ready`; the desktop creates the file
  only after that F8-triggered first full composition and removes it on every
  switch back to a text VT. On startup, the desktop removes stale status files;
  a missing frame file is treated as frame zero.
- `mario-desktop` atomically rewrites `/tmp/mario-desktop.frame` with a
  monotonically increasing decimal frame number after every completed
  composite. The runner snapshots the prior number before injecting an action
  and waits for a larger one; an abandoned `-EBUSY` flush does not update it. Add
  `GUI-WAIT-FRAME`, which waits through the serial console until that number
  advances; use it after each input action before taking a screendump. Never
  use `SLEEP:` to synchronize a GUI test.
- Add `GUI-MOUSE-MOVE: dx,dy` and `GUI-MOUSE-BUTTON: button,state` directives.
  The runner owns the QEMU HMP/QMP spelling, so test files describe only Mario
  input actions. Tests inject real PS/2/QEMU events and never write directly
  to `/dev/gui` or `/dev/psaux`.
- Add `GUI-CHECK-SCREEN-RECT: golden.ppm,x,y,w,h` for arbitrary pixel crops;
  the existing row-based crop directive remains for text-console tests. GUI
  PPM comparisons are pixel-exact at 1024x768x32. The pinned theme, fonts,
  static `--:--` clock, fixed cursor, and deterministic fixtures make a
  tolerance unnecessary; a changed golden requires explicit review.
- Split coverage by layer: shell tests cover framebuffer/input record ABIs and
  error paths; GUI tests cover visible desktop behavior; each GUI test
  drives the real desktop, widgets, PTYs, and applications end to end.
- Keep one focused visual golden per behavior: `gui.desktop.test` covers mode
  switching and retained desktop state; `gui.window.test` covers focus, move,
  resize, and close; `gui.terminal.test` covers PTY resize, direct RGB, and
  tmux panes; `gui.imageviewer.test` covers BMP/PNG zoom and pan; and
  `gui.filemanager.test` covers real `/tmp` mutations plus launch behavior.
- On failure, retain the actual screendump and append the serial output of
  `cat /tmp/mario-desktop.debug`; that file reports foreground mode, frame
  number, focused window, pointer position, and the last 32 GUI events.
- Tests that open PTYs clean up by closing the master/slave and waiting for
  child exit; the harness also kills the QEMU instance so leaked PTYs do not
  affect the next test.
- GUI tests share the same read-only hard-disk images as shell tests. Any
  GUI test that creates files uses a scratch directory under `/tmp` and does
  not modify `/dev/hdc*` images.

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
- Existing TTY tests depend on the 80x25 text-console behavior. Preserve that
  behavior and its tmux panel baselines incrementally.
- The custom `boot.bin` path does not yet supply VBE. Do not delete the text
  console path until that loader is upgraded.
- Mouse IRQ 12 can conflict with legacy PIC assumptions. Disable and fall back
  to keyboard-only operation if no mouse is detected.
- PTY work touches the tty line discipline used by all consoles. Keep the
  console path working with the existing shell and TTY tests before adding
  GUI terminal features.
- Keep the single-process GUI shippable while the window-manager protocol and
  client lifecycle mature; the underlying `AF_UNIX` transport is already
  covered by its own socket and tmux regressions.
- `/dev/fb0` mmap requires physical-device VMA support and must not be
  required by the first GUI. The first implementation uses read/write/pwrite.
- TTF glyph caches can grow quickly at many point sizes. Cap cache entries and
  evict least-recently-used glyphs rather than caching every codepoint.
