# GUI Desktop and Applications

Part of the [GUI platform plan](README.md). This document defines the
compositor, terminal integration, initial applications, and source layout.


## Process Model

Phase 1:

- `mario-desktop` is a single process.
- It owns the framebuffer and input devices.
- It creates windows directly. M3 provides built-in desktop diagnostic views;
  later milestones add the terminal, image viewer, and file manager as
  internal views. The initial desktop uses one `poll()` loop and no worker
  threads.

Phase 2:

- `mario-wm` is the GUI server and compositor.
- Applications are separate processes connected by `AF_UNIX` sockets.
- `mario-desktop` is the shell/panel/launcher client.

This document uses the Phase 2 layout as the target, with Phase 1 as the first
shippable integration point.

## Window Manager Responsibilities

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
- Alt-Tab cycles through the most-recently-used order, not z-order. Super-W
  sends `WINDOW_CLOSE` to the focused window.
- Input not claimed by a popup/modal grab goes to the window under the
  pointer. Modal dialogs are deferred to Phase 2; Phase 1 has no modal
  windows.

## Desktop Shell

The desktop client provides:

- a solid background;
- a bottom panel with app launcher, window list, and a fixed `--:--` clock
  placeholder;
- taskbar buttons for open windows;
- Alt-Tab and Super-W keyboard shortcuts; Super-Return begins launching or
  raising the terminal only in M5;
- desktop icons and wallpaper configuration are deferred beyond the first
  desktop milestone.

The first desktop uses a fixed testable theme: background `#1f2933`, 28-pixel
bottom panel `#111827`, 24-pixel title bars, 1-pixel borders `#0f172a`, active
title bars `#2563eb`, and inactive title bars `#475569`. The bundled 16x16
arrow cursor is the topmost layer. The static `--:--` panel clock avoids
depending on an RTC or test-only environment setup; real time is deferred.

M3 creates two real built-in diagnostic windows on every fresh desktop start:

- `Welcome` at `(96, 96)`, sized `360x220`, with the desktop name and shortcut
  summary;
- `Display` at `(480, 220)`, sized `320x192`, with the framebuffer dimensions
  and the current focus state.

They use the normal window-manager, compositor, focus, move, resize, and
close paths; they are not a test-only input or framebuffer bypass. Their fixed
initial geometry makes `gui.desktop.test` and `gui.window.test` meaningful
before the terminal and other applications arrive. Before M5 the launcher
contains only `Welcome` and `Display`; it raises an existing diagnostic window
or recreates a closed one through the same normal window-creation path.

## Compositing Strategy

The first compositor is simple:

1. Collect dirty rectangles.
2. Clip each dirty rectangle to each visible window from back to front.
3. Blit only the visible portions.
4. Blit the cursor sprite last.
5. Flush through `pwrite()` to `/dev/fb0`.

The compositor composes at most once per 16 ms timer tick and only when it has
damage. It may use a full-screen composite in M3, but the flush routine must
write each damaged scanline with a retrying `pwrite()` loop that handles short
writes and `EINTR`. If a flush returns `-EBUSY` because graphics mode was
switched away, it abandons that frame, keeps the accumulated damage, and waits
for `/dev/fb0` `POLLPRI`; it never advances the completed-frame marker for an
abandoned composition. Vsync is not required.

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
8. Window resizes send `TIOCSWINSZ` to the retained master and redraw the
   terminal grid.

## VT Emulator Scope

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

The terminal advertises `TERM=xterm-direct` and `COLORTERM=truecolor`.
`xterm-direct` is the ncurses direct-color profile derived from xterm and is
installed with its terminfo entry. It supports ANSI 16-color SGR, indexed
`38;5;n`/`48;5;n` colors, and direct RGB `38;2;r;g;b`/`48;2;r;g;b` plus the
colon-separated form used by the terminfo profile (`38:2::r:g:b` and
`48:2::r:g:b`). Terminal cells retain direct foreground and background
`0xRRGGBB` values rather than reducing them to the 256-color palette. Invalid
RGB components or incomplete SGR groups are ignored without changing the
current color. It also supports `CSI c` with the VT102 `?6c` response,
`CSI 5n`/`CSI 6n` status reports, `CSI ?25h`/`CSI ?25l` cursor visibility, and
`CSI ?1049h`/`CSI ?1049l` alternate-screen switching. It does not implement
mouse reporting, bracketed paste, or OSC clipboard.

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
  minus scrollbar/chrome, then calls `TIOCSWINSZ` on the retained master with
  row/column counts. The grid is reflowed only for full-screen width changes;
  soft reflow is deferred.
- Set the shell environment before exec: `TERM=xterm-direct`,
  `COLORTERM=truecolor`, `COLUMNS` and `LINES` equal to the initial grid size,
  `HOME=/root`, and `SHELL=/bin/oksh`. Update `COLUMNS`/`LINES` through
  `TIOCSWINSZ` on resize; `oksh` reads the tty size for `stty size`.
- Child termination: when the shell exits, the terminal prints a status line
  such as `[process exited]`, keeps the scrollback visible, and closes on
  explicit window close or Enter.

## Applications

### Image Viewer

Add `app/usr/imageviewer/`.

Features:

- open an image from a path or from a file-manager launch;
- decode BMP and PNG;
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

- A GUI test opens a known PNG/BMP, dumps the screen, and checks that the
  image is visible and the status line is correct.

### File Manager

Add `app/usr/filemanager/`.

Features:

- navigate `/`, `/root`, `/mnt`, and mounted ext2/vfat directories;
- display entries with name, type, size, and modification time;
- change directory, parent directory, and absolute path entry;
- open files with the matching application:
  - directories: navigate;
  - text files: a terminal running `less`;
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
  successful copy. Refresh also after a child app exits and after the manual
  Refresh command.
- Text files open in the terminal app with `less` by default; image files open
  in the image viewer; executables run in a new terminal window with a shell
  command line quoted for `oksh`.
- When a launch target cannot be executed, keep the file manager open and show
  the failure in a status line/message rather than silently closing.

Validation:

- Shell/GUI tests exercise file-manager actions on both mariofs and an
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

- Phase 1 extends `app/system/init/main.c`: after `/usr` is mounted, init
  checks for `/usr/bin/mario-desktop`, forks it, and records its pid alongside
  the gettys. The child calls `setsid()` before `exec`; init waits three
  seconds before restarting a reaped desktop child. The common image always
  contains the desktop binary; if `/dev/fb0` is unavailable, it exits with
  status 77 and init leaves the text session running without respawning it.
- `mario-desktop` opens `/dev/fb0` for exclusive write access, then claims the
  exclusive read-only `/dev/gui` seat, then opens `/dev/psaux`. On any later
  failure it closes already-opened fds and exits before enabling F8. It then
  remains dormant while a text VT is foreground; F8 makes it redraw the
  desktop after the `POLLPRI` notification. The desktop process is the session
  leader for GUI apps.
- Phase 2 installs `/usr/bin/mario-wm`. Init prefers that session binary over
  `/usr/bin/mario-desktop`; `mario-wm` then binds the socket, owns the raw
  devices, and spawns `mario-desktop` as its first client. App exit does not
  terminate the WM; WM exit or a killed server closes all client windows and
  returns to the text console.
- Keep initial settings in `/etc/mario-gui.conf` as `key=value` lines:
  background color, font paths, terminal command, panel position, and
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
src/fs/char/gui.c
src/fs/char/pty.c
src/fs/char/psaux.c
src/fs/char/tty.c          # refactor for shared line discipline
src/include/fs/tty/pty.h
src/include/fs/gui.h
src/include/fs/psaux.h
src/include/fs/tty/tty.h
src/kernel/start.S
src/include/multiboot.h
src/mm/paging.c
```

Userspace:

```text
app/usr/libmario-ui/
app/usr/desktop/
app/usr/terminal/
app/usr/imageviewer/
app/usr/filemanager/
app/usr/launcher/
app/usr/Makefile
app/usr/freetype/
app/usr/libpng/
app/usr/libmario-ui/assets/fonts/  # bundled TTF source files
rd/etc/mario-gui.conf      # simple GUI session configuration
rd/device_list             # adds fb0, gui, psaux, ptmx, pts/0..31
```

Test:

```text
tests/gui/fb.test
tests/gui/gui.desktop.test
tests/gui/gui.terminal.test
tests/gui/gui.imageviewer.test
tests/gui/gui.filemanager.test
tests/gui/gui.font.test
tests/shell/pty.test
tests/shell/mouse.test     # read-only packet smoke test if device present
```
