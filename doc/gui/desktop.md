# GUI Desktop and Applications

Part of the [GUI platform plan](README.md). This document defines the
compositor, terminal integration, initial applications, and source layout.

## Scope and Authority

This is the normative behavior contract for the desktop shell, compositor,
terminal, and first applications. Kernel ABI and synchronization rules remain
owned by [Kernel foundations](kernel-foundations.md); shared drawing/event API
rules remain owned by [UI toolkit](ui-toolkit.md). The
[implementation roadmap](roadmap.md) alone defines implementation order,
completion state, test gates, and commit boundaries.

### Session State

`mario-wm` has one explicit foreground-session lifecycle:

1. **Dormant:** on a framebuffer-capable boot, init has started the server,
   but F8 has not produced a graphics foreground transition. Text-only boots
   remain getty-only. The dormant server has already reserved writable
   `/dev/fb0` and read-only `/dev/gui`, so F8 is eligible, but performs no
   framebuffer write and exposes no readiness file. It listens for clients;
   `mario-desktop` connects as the first shell client.
2. **Active:** after F8 and the corresponding `/dev/fb0` `POLLPRI`, graphics
   is foreground; the server composes committed client surfaces and flushes
   frames. It creates
   `/tmp/mario-wm.ready` only after the first completed frame.
3. **Suspended:** F1–F6 makes text foreground. The process keeps retained
   client/window state, the reserved seat, and accumulated damage; removes
   `ready`; and does not publish a completed frame until a later F8 transition
   succeeds.
4. **Shell recovery:** if the private shell endpoint closes, the server removes
   its normal shell windows but retains the last committed desktop surface as an
   input-inert snapshot. Ordinary application windows and clients remain live.
   The server immediately starts one replacement shell; the replacement gets a
   fresh window-list snapshot and atomically replaces the retained desktop
   pixels after its first desktop-surface commit. If that replacement cannot
   become shell-ready (`HELLO` plus a full desktop-surface commit), the server
   ends the session rather than retrying in a loop.
5. **Shutdown/error:** it removes all status files, closes the original seat
   descriptors and client connections, and leaves the kernel to restore the
   remembered text VT.

The server never treats a `POLLPRI` wakeup alone as an instruction to draw; it
first reads `FBIOGET_CONSOLE_STATE`. Clients never open `/dev/fb0` or use
foreground state as a rendering permission.

## Process Model

- `mario-wm` is the GUI server and compositor.
- It owns the framebuffer, raw-input devices, foreground transitions, and the
  pathname `AF_UNIX` listener.
- Applications are separate processes connected to the public `AF_UNIX`
  listener.
- `mario-desktop` is the sole shell/panel/launcher client. The server creates
  a private `socketpair()` endpoint for it before `exec`, which is the only
  shell capability; `HELLO` never carries a client-claimed shell role.
- The shell owns one non-decorated, bottom-layer desktop surface covering the
  scanout. It draws the background, panel, taskbar, launcher, and fixed clock
  into that surface; normal windows remain server-composited above it.
- On shell exit, the server keeps ordinary clients/windows and the last
  committed desktop snapshot while it starts one replacement shell. The
  replacement uses a new private endpoint and never inherits the old one.
- M3 starts the server and shell client together; M5–M7 add terminal,
  image-viewer, and file-manager clients without a later transport migration.
- The initial server and clients each use one `poll()` loop and no worker
  threads.

## Window Manager Responsibilities

- Maintain z-order and focused-window state.
- Own all framebuffer drawing after the GUI console becomes active.
- Route keyboard and mouse events.
- Provide window move, resize, raise, close, and title-bar hit testing.
- Clip dirty rectangles to the visible region of each window.
- Render window chrome and the mouse cursor.
- Send cooperative close requests and resize events to applications.
- Redraw the whole desktop on GUI-console activation.
- Intercept Alt-Tab, Super-W, and Super-F before client input, and publish ordered
  window-list add/change/remove notifications only to the shell capability.

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
  sends one cooperative `WINDOW_CLOSE_REQUEST` to the focused window. The
  client keeps the window until it sends `WINDOW_DESTROY` or disconnects; M3
  has no forced-close timeout.
- Input not claimed by a popup/modal grab goes to the window under the
  pointer. Modal dialogs are deferred beyond the first desktop; initial
  clients have no modal windows.

### Expanded Capacity and Fullscreen (M9 Target)

M9.1 implements the expanded surface pool, admission reservations, shell
replacement headroom, and recoverable transition rollback. M9.2 implements
resource-bounded dynamic connection/window/launch/poll records, multiple
windows per ordinary connection, and a dynamic shell window list. M9.3 implements
taskbar paging and large snapshot/delta recovery. M9.4 implements allocation-free
union occlusion and bounded atomic marker publication. Fullscreen remains planned
in subsequent M9 steps.

- Replace fixed client/window arrays with dynamically allocated records in
  the WM and shell. There is no small fixed window-count or connection-count
  ceiling, and no promise of literally unlimited windows: admission is bounded
  by checked memory budgets, available file descriptors/processes, and stable
  ID space. Pending launch records grow under the same resource policy and
  failed launches are reaped. Share the byte-budget policy with regression
  clients; never turn a minimum tested count into an implementation maximum.
  Surface admission follows the
  [toolkit surface-budget contract](ui-toolkit.md#surface-memory-and-failure-handling).
  M9.2 charges actual WM record and poll capacity to 1 MiB, including a
  256 KiB share per connection, and shell model records to a separate 1 MiB.
  Kernel descriptor/process limits and the independent protocol queues still
  apply. Admission/allocation failure closes only a rejected new connection
  or rejects a new window without discarding healthy windows.
- Allow multiple ordinary windows on one application connection using the
  existing request/window IDs. Store geometry, committed/staging surfaces,
  configure serials, focus, grabs, and fullscreen state per window rather than
  per connection. Closing one window must not close its siblings; disconnect
  reclaims every window owned by that endpoint. Validate ownership on every
  window-addressed message. Single-window clients remain compatible, and a
  connection-scoped `INPUT_RESET` clears input state for all its windows.
  `wm-window-lifecycle.test` exercises 40 siblings and 40 independent peers,
  interleaved stages, per-window resize/close, stale and foreign IDs, shell
  replacement, and admission after teardown. Initial shell snapshots advance
  in batches of at most eight records only when the output queue is empty;
  mutations to unvisited IDs appear in their eventual snapshot, while deltas
  to visited IDs follow their queued snapshot.
- At 1024x768 on the normal 256 MiB test guest, support at least 16 additional
  320x192 application windows while retaining the desktop and diagnostics.
  This is a minimum regression workload, not a maximum. Separate growth tests
  keep at least 40 small windows on one connection and at least 40 simultaneous
  connections, crossing the old eight-entry and proposed 32-entry limits.
  Also support four near-screen-sized normal windows at the maximum decorated
  content dimensions, with replacement frames, retaining the desktop and
  diagnostics. The application and large-window workloads run separately;
  both must allow repeated desktop and application updates, not just creation.
- Keep finite per-client and session surface/record budgets, independent
  input/output queue limits, and bounded per-turn work. Exhausting a budget or
  kernel resource rejects only the new operation/connection, without killing
  healthy existing windows. Fully obscured windows need no composition or
  scanout work, but their retained backing stores still count against memory
  budgets; M9 does not add swapping or discard hidden content.
- Occlusion uses the union of higher opaque decorated rectangles clipped to
  scanout, with allocation-free vertical bands and horizontal coverage scans.
  Fully covered layers are omitted from composition. A complete hidden upload
  replaces retained pixels without composing, flushing, or advancing the
  physical frame counter. Incomplete uploads and rejected commits keep prior
  pixels and all staging charges. Moving or closing covers and returning from
  a text VT reveal the latest complete upload. `wm-occlusion.test` checks
  adjoining covers, partial staging, and F8 reveal; the WM link gate also checks
  skipped scanout, allocation failure, and exact retained-byte accounting.
- Atomic status publication retries MarioFS `EBUSY` reader conflicts at most
  sixteen times with 10 ms between attempts. A releasing reader sees its intact
  old counter while publication proceeds after it closes; a persistently held
  reader exhausts the bounded retry, preserving the old counter and removing
  the incomplete publication's temporary file. The WM link tests both paths.
- Super-F toggles the focused ordinary window between normal and fullscreen.
  The WM owns the shortcut; no new client protocol message is required.
  Fullscreen content occupies `(0,0)` through the full scanout dimensions,
  without title bar, borders, or panel; the cursor remains visible. Save the
  exact normal rectangle once on entry and restore it on exit, clamping only
  if screen bounds require it. Fullscreen is distinct from maximization;
  maximization, display-mode changes, and multiple monitors remain deferred.
- Only one window is fullscreen at a time. Alt-Tab or a shell activation of
  another window restores the old fullscreen window before activating the
  target; Super-F on the desktop is a no-op. Super-W remains a cooperative
  close request. Pointer coordinates become fullscreen-content-local, and
  fullscreen edges do not start a decoration move/resize. Preserve ordered
  input resets on every focus/grab/seat transition.
- Use normal configure serials and surface transactions for entry and exit.
  Reject insufficient capacity before changing mode; a later allocation or
  commit error restores the prior state without publishing partial pixels.
  Present the previous mode and exact rectangle until a complete matching
  upload commits, suppressing client input while the mode is pending. Reject
  ordinary move/resize and overlapping mode requests during that transaction.
  Restoring always composes, even if the restored window is fully obscured, so
  old fullscreen pixels cannot linger. Keep the fullscreen owner through a
  pending exit and defer another window's activation by stable ID until the
  normal-size pixels commit. Failed entry/exit releases new reservations and
  staging pixels, preserves old committed charges, resets input, and publishes
  a fresh configure for the restored mode.
  Automatically created windows, including replacement-shell diagnostics, do
  not exit fullscreen or steal focus; creation still reports an unfocused
  `WINDOW_FOCUS` followed by `INPUT_RESET` to preserve client handshakes.
  `wm-fullscreen.test` compares actual fullscreen/restored QMP frames and checks
  partial uploads, stale serials, screen-edge clicks, cooperative sibling close,
  seat switches, and shell replacement. The WM link also forces admission,
  allocation and scanout failures and verifies geometry, focus and exact charges.
  A successful terminal configure propagates through `TIOCSWINSZ` and
  `SIGWINCH` to tmux and its panes. F1-F6/F8 preserve fullscreen state and
  restore the last text VT normally. Fullscreen-client exit restores the
  desktop; shell replacement preserves the surviving application's mode.

## Desktop Shell

The desktop client provides:

- a solid background;
- a bottom panel with app launcher, window list, and a fixed `--:--` clock
  placeholder;
- taskbar buttons for open windows;
- taskbar-driven activate/raise requests and Super-Return launch requests;
  `mario-wm`, not the shell, owns Alt-Tab and Super-W;
- a dynamically allocated window list with deterministic
  previous/next taskbar pages when buttons do not fit. Preserve launcher and
  clock space, keep every window reachable, and reveal the focused window's
  page automatically. Bind clicks to stable window IDs rather than recycled
  array positions; removal and shell-recovery snapshots cannot leave stale
  buttons. Stream large snapshots and ordered deltas within the existing
  bounded output queue rather than enqueueing the whole list at once. The
  panel is hidden only while an ordinary window is fullscreen;
- desktop icons and wallpaper configuration are deferred beyond the first
  desktop milestone.

At 1024x768 the unpaged taskbar fits nine windows. Overflow uses eight
stable-ID buttons per page, with previous/next controls outside the launcher
and clock. A newly focused window reveals its page; clicks resolve against
the last submitted button table and validate the ID against the current model.
The button tables and model records share the shell's metadata budget.
Snapshots freeze their upper window ID and stream at most eight records per
empty output queue. Changes coalesce until delivery; removals retain charged
tombstones until their ordered notification is queued. A cyclic delta cursor
prevents a continuously changing window from starving later additions. Shell
replacement reclaims this state and starts a fresh bounded snapshot.
Metadata changes that do not change the taskbar's visible model do not trigger
a shell repaint, so a busy title writer cannot force redundant full frames.
Valid keyboard events directed to the focused shell are ignored when they have
no shell binding; they do not terminate its private connection.
The protocol workload reports fatal assertions and exits unsuccessfully instead
of entering an unsupported abort loop. Readiness waits have bounded per-action
deadlines, so a failed fixture does not spend repeated three-minute timeouts
before reporting its log.
Freed MarioFS block chains restore the free-block admission counter, so repeated
atomic frame-marker replacement does not falsely exhaust the root ramdisk.

The first desktop uses a fixed testable theme: background `#1f2933`, 28-pixel
bottom panel `#111827`, 24-pixel title bars, 1-pixel borders `#0f172a`, active
title bars `#2563eb`, and inactive title bars `#475569`. `mario-wm` owns the
bundled 16x16 arrow cursor as the topmost layer. The static `--:--` panel clock avoids
depending on an RTC or test-only environment setup; real time is deferred.

On every fresh desktop start, the `mario-desktop` shell client creates two real
diagnostic windows through `mario-wm`'s normal protocol framing:

- `Welcome` at `(96, 96)`, sized `360x220`, with the desktop name and shortcut
  summary;
- `Display` at `(480, 220)`, sized `320x192`, with the framebuffer dimensions
  and the current focus state.

They use the normal server composition, focus, move, resize, and close paths;
they are not a test-only input or framebuffer bypass. Their fixed initial
geometry makes `gui-desktop.test` and `gui-window.test` meaningful before the
terminal and other applications arrive. Before M5 the launcher contains only
`Welcome` and `Display`; it asks the shell client to raise an existing
diagnostic window or create a replacement through the same protocol framing.

## Compositing Strategy

Through M9, `mario-wm` can eagerly compose and flush the full screen for each
visible update or pointer movement. Window contents are already cached in
committed surfaces; moving a window does not require regenerating its text or
widgets. The incremental sequence below is the M10 target, not a claim that
damage-based presentation or frame pacing is already implemented. Paint the
shell desktop surface first, then normal windows/chrome, then the cursor:

1. Collect dirty rectangles.
2. Clip each dirty rectangle to each visible window from back to front.
3. Blit only the visible portions.
4. Blit the cursor sprite last.
5. Flush through `pwrite()` to `/dev/fb0`.

Under M10, motion-only updates share a presentation on a 16 ms cadence and
produce no scanout when they leave the visible state unchanged. Client commits
retain their existing transactional error handling; motion pacing must not
silently defer a commit's rollback decision. The flush routine writes damaged
scanlines or contiguous spans with a retrying `pwrite()` loop that handles short
writes and `EINTR`. If a flush returns `-EBUSY` because graphics mode was
switched away, it abandons that frame, keeps the accumulated damage, and waits
for `/dev/fb0` `POLLPRI`; it never advances the completed-frame marker for an
abandoned composition. Vsync is not required. M3 does not begin until the
kernel's positional-I/O gate is green: `pwrite64()` must preserve `f_pos`,
handle short writes, and pass the framebuffer-specific regression tests.

A valid client surface commit is independent of physical presentation. If a
VT transition interrupts its composite with `GUI_FLUSH_BUSY`, the server
retains the newly committed surface, releases the prior one, and recomposes
the latest state on the next foreground notification. It does not roll the
transaction back or send `SURFACE_COMMIT` `-EBUSY` to a client. The completed
frame counter and readiness marker still advance only after a successful
scanout; a real flush error retains the prior committed surface and reports
the transaction error.

### Incremental Drawing (M10 Target)

- Keep a persistent composite and reuse each window's newest complete cached
  surface. Accumulate bounded screen-space damage for content, chrome, focus,
  stacking, geometry, lifecycle, desktop, and cursor changes. Clip all drawing
  operations to that damage, not merely the final framebuffer writes.
- A move invalidates the old and new decorated bounds. Copy cached window
  pixels at the new location and reconstruct exposed pixels from the desktop
  and lower windows in stacking order. Unaffected areas remain untouched;
  partially covered windows cannot be copied over higher windows. Restoring
  only the exposed strips is insufficient: destination window pixels still
  need copying to the framebuffer. Screen-to-screen hardware blitting is not
  assumed.
- Pointer-only movement damages the old and new cursor bounds, including
  clipping at screen edges. Restore those pixels from the scene and blend the
  cursor last. Cursor trails, stale save-under pixels, and cursor contamination
  of window caches are failures. A fixed, accounted sprite-sized save-under
  buffer is permitted if scene updates invalidate it correctly; a second
  unaccounted full-screen snapshot is not.
- Process input state in order, including each relative motion's edge
  clamping, button transitions, input resets, capture changes, and keys. Batch
  only physical drawing. Track the last presented geometry/cursor footprint
  and latest pending destination; do not accumulate damage for intermediate
  positions that were never displayed. Use bounded input/client dispatch and
  an explicit next-frame deadline so a continuously readable mouse or busy
  peer cannot starve presentation, other clients, timers, or cooperative close.
- A completed-frame marker advances once per successful nonempty presentation,
  not per input record or hidden logical commit. Preserve damage on partial
  failure; a foreground transition invalidates the entire physical scanout.
  F8 return, initial display, and lost scanout state therefore require a full
  repaint. Real flush errors keep the existing surface/geometry rollback and
  session-failure contracts; `GUI_FLUSH_BUSY` remains a logical commit success.
- Terminal rendering tracks changed cells and the old/new terminal cursor,
  preserving cached glyphs and unchanged pixels. After the separately
  negotiated rectangular-upload extension exists, send only a coalesced dirty
  rectangle, with a full frame for initial display, geometry changes, or base
  resynchronization. Non-negotiating applications retain full-frame uploads.
- Incremental uploads remain atomic, bounded, and tied to window, configure,
  base-frame, and new-frame identities. The terminal uploader services input,
  PTY output, configures, and replies while a bounded number of data chunks
  are outstanding; it does not add a private shell commit-ack protocol or
  bypass existing peer/session budgets.

## Terminal Window and PTY Integration

Add:

- `app/usr/terminal/main.c`
- `app/usr/terminal/term.c`
- `app/usr/terminal/vt.c`

The terminal client:

1. Discards inherited nonstandard descriptors, then connects to `mario-wm`,
   creates its window, and submits surface transactions.
2. Opens `/dev/ptmx`.
3. Calls `grantpt`/`unlockpt` equivalents via `TIOCSPTLCK` and `TIOCGPTN`.
4. Opens `/dev/pts/N`.
5. Forks a shell child. The child calls `setsid`, opens the slave as its
   controlling tty, duplicates it to stdin/stdout/stderr, and execs
   `/bin/oksh`. Before `exec`, it closes the terminal's window-manager socket
   and every inherited framebuffer, raw-input, mouse, and unrelated PTY
   descriptor; only the slave-backed standard streams remain. It restores
   the default `SIGHUP` disposition rather than inheriting init's ignored
   hangup signal, so closing the master terminates the child. A shell must
   never keep the graphics seat, client connection, or PTY slave alive after
   the desktop or terminal exits.
6. The terminal client keeps the master and `mario-wm` socket fds in its
   `poll()` set.
7. Master bytes are parsed by the VT emulator and written to the terminal
   window surface.
8. Server-delivered key events are translated into escape sequences and written
   to the master.
9. Server-delivered window resizes send `TIOCSWINSZ` to the retained master and
   redraw the terminal grid.

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
- Keyboard controls follow common terminal-viewer conventions: `f` returns to
  fit, `1` selects actual size, and `+`/`=` and `-` move through the bounded
  zoom levels. Arrow keys and Vim-style `h`, `j`, `k`, and `l` pan the clipped
  viewport; a left-button drag moves the image directly. `n`/Page Down and
  `p`/Page Up select the next and previous supported sibling in `readdir()`
  order.
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
- Text files request a terminal app with `less` by default; image files request
  the image viewer; executables request a new terminal with a shell command
  line quoted for `oksh`. `mario-wm` validates the bounded request, forks the
  approved client binary, and reaps it; the file manager never directly forks
  another GUI client.
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

The launcher is a grid of buttons above the panel. It sends typed launch
requests to `mario-wm`; the server starts only the approved initial client
binaries and reaps them. A shell child never inherits raw-seat, listener, or
shell-capability fds. File-manager launches use the same server request path.

Session lifecycle and configuration:

- M3 extends `app/system/init/main.c`: after `/usr` is mounted, init checks for
  `/usr/bin/mario-wm`, forks it, and records its pid alongside the gettys. The
  child calls `setsid()` before `exec`; init waits three seconds before
  restarting a reaped server. The common image always contains the server;
  init checks whether `/dev/fb0` is available before starting it and leaves
  text sessions running without a graphics service on a no-VBE boot. A manual
  server launch without a framebuffer fails with status 1 and creates no
  socket or session markers.
- After an unexpected exit of its supervised server, init removes its socket,
  started/ready/frame markers, and temporary marker files before attempting
  any restart. This cleanup does not depend on a successful replacement exec.
  The dead server's last raw-device close restores the saved text VT; a
  replacement reserves a new dormant seat and never synthesizes F8. An orderly
  server removes its paths while it still owns the exclusive seat, so its
  cleanup cannot unlink a successor's listener or completed-frame markers.
- `mario-wm` opens `/dev/fb0` for exclusive write access, claims the exclusive
  read-only `/dev/gui` seat, then opens `/dev/psaux` and binds
  `/tmp/mario-wm.sock`. On any later failure it closes already-opened fds,
  removes the socket/status files, and exits before enabling F8. It remains
  dormant while text is foreground; F8 makes the server composite committed
  client surfaces after the `POLLPRI` notification.
- After binding the public listener, the server creates a private `socketpair()`,
  forks `/usr/bin/mario-desktop --wm-fd=3`, and passes only one connected
  endpoint at fd 3 to the child. That endpoint grants the shell capability;
  public-listener clients can never request it. A normal application exit or
  launch failure does not terminate the server; `LAUNCH_RESULT` reports the
  error to the requesting client. If the initial shell cannot become shell-ready
  (`HELLO` plus a full desktop commit), or if a replacement shell fails, the
  server exits to text. On later shell exit,
  the server retains ordinary clients plus the last desktop snapshot and
  immediately starts one fresh shell endpoint. Server exit or a killed server
  closes all client windows and returns to the text console. Children inherit
  no framebuffer, raw-input, listener, or unrelated client fd.
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
app/gui/libmario-ui/
app/gui/desktop/wm_main.c
app/gui/desktop/wm_*.c
app/gui/desktop/shell_main.c
app/gui/desktop/shell_*.c
app/gui/libmario-ui/include/gui/protocol.h
app/usr/terminal/
app/usr/imageviewer/
app/usr/filemanager/
app/usr/launcher/
app/usr/Makefile
app/usr/freetype/
app/usr/libpng/
app/gui/libmario-ui/assets/fonts/  # bundled TTF source files
rd/etc/mario-gui.conf      # simple GUI session configuration
rd/device_list             # adds fb0, gui, psaux, ptmx, pts/0..31
```

Test:

```text
tests/gui/fb.test
tests/gui/gui-desktop.test
tests/gui/gui-terminal.test
tests/gui/gui-imageviewer.test
tests/gui/gui-filemanager.test
tests/gui/gui-font.test
tests/shell/pty.test
tests/shell/mouse.test     # read-only packet smoke test if device present
```
