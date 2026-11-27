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
- Double-clicking a window's title bar toggles maximize/restore using the same
  saved geometry as its maximize button. Left clicks must be on the same window
  within 500 ms and four pixels; drags beyond that tolerance, other clicks,
  keyboard input, and input resets cancel the sequence.
- Hovering resize grips selects horizontal, vertical, or matching diagonal
  cursors. Title drags use a move cursor. A captured drag keeps its cursor even
  outside the original grip, then returns to the current hover cursor on release
  or input reset. Resize/move hot spots stay centered on the logical pointer;
  the normal arrow retains its top-left hot spot. Shape changes redraw both old
  and new cursor bounds, including when the pointer itself does not move.
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

- an optional PNG/BMP wallpaper, with a solid background fallback;
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
- desktop icons and a graphical wallpaper chooser are deferred beyond the first
  desktop milestone.

Set `MARIO_WALLPAPER` to an image path inside the guest before starting
`mario-wm`, for example:

```sh
MARIO_WALLPAPER=/root/background.png /usr/bin/mario-wm
```

When the variable is unset, the shell uses the `wallpaper` setting in
`/root/.config/mario/desktop.conf`, defaulting to `/usr/share/mario/wallpaper.png`.
An empty `wallpaper=` setting disables the image. The environment variable
takes precedence over the file; an empty environment value also disables it.
See [Desktop Appearance](appearance.md) for configuration paths and examples.
To install a persistent default from the host, place a PNG at
`rd/_ramdisk0/usr/share/mario/wallpaper.png` before rebuilding the disk image.
No default image is bundled, so the existing solid theme remains unchanged.

The shell recognizes PNG/BMP by their contents, decodes once per shell start,
and caches the source pixels plus compact row/column sampling maps. Nearest-neighbor
scaling preserves aspect ratio and center-crops to cover the whole display;
the panel and launcher are painted over it. Ordinary repaints do not reload the
file or allocate another full-screen image. Shell replacement reloads the same
configuration. Missing, invalid, unsupported or over-limit files, and allocation
failures, fall back to the solid theme without terminating the desktop.
The toolkit's decoder limits apply: 64 MiB input, dimensions at most 4096,
and 16 Mi pixels. PNG support is non-interlaced 8-bit RGB/RGBA; alpha is flattened
against black, as in the image viewer. BMP support is 24-bit uncompressed RGB.

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

See [Desktop Appearance](appearance.md) for configurable font families, sizes,
and desktop/window-decoration color schemes.

Without appearance configuration, the desktop uses its original theme:
background `#1f2933`, 28-pixel
bottom panel `#111827`, 24-pixel title bars, 1-pixel borders `#0f172a`, active
title bars `#2563eb`, and inactive title bars `#475569`. `mario-wm` owns the
bundled 16x16 arrow or a state-specific built-in cursor as the topmost layer.
The static `--:--` panel clock avoids
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
widgets. M10 now implements damage-based presentation, bounded motion pacing,
cell reuse and bounded negotiated uploads. Paint the
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

M10.2 implements bounded composition and scanout. The WM retains its composite
and a fixed 64-rectangle region; overflow conservatively invalidates the screen.
No additional production image or cursor save-under allocation is required.
Window presentation bookkeeping is included in the existing charged window
records. Each damaged rectangle is a clipped view of the composite, including
the final cursor stores, so scene reconstruction cannot touch unrelated pixels.
Damage and last-presented geometry are retained until every rectangle is flushed
and the completed-frame marker is published. Hidden commits retain their cached
surface without scanout; foreground return invalidates all physical pixels.
M10.3 applies motion records immediately but schedules their physical drawing
at the first pending motion's monotonic 16 ms deadline. Later motion replaces
the pending destination without extending that deadline or damaging unseen
intermediate positions. Button transitions and client transactions retain
immediate presentation; ordinary keys are dispatched without waiting for the
motion deadline. Each keyboard/mouse drain reads at most four 16-record batches,
and deadlines are checked between batches and between bounded client drains.
Poll timeouts include the pending deadline even without further input. BUSY
scanout retains damage and suppresses deadline polling until foreground
notification or a successful immediate transaction, avoiding an expired-timer
spin.

M10.5 retains terminal pixels and an explicitly sized prior-cell snapshot
(`rows * columns * sizeof(terminal_vt_cell)`, 5760 bytes for the 320x192 grid).
Cells are compared by fields rather than padding. Changed cells, old/new cursor
cells, and old/new glyph bounds form one clipped dirty bounding rectangle;
only cell backgrounds and glyphs intersecting it are reconstructed, in the
original row/column order. Glyph overhangs are included, so clipping repairs
neighboring cells and margins without changing full-render pixels. Scrolling,
alternate screens, erases, and widely separated changes conservatively grow
the rectangle; unchanged state does not draw. New geometry forces a full
render. Replacement grids, snapshots, and pixels are allocated before
discarding the working renderer. Allocation failure preserves it and defers
the pending configure through the existing bounded surface retry timer.
M10.6 adds v1.1 atomic patches; M10.7 pipelines four acknowledged chunks within
a 16 KiB encoded-data window, while continuing PTY, key, and configure service.
The terminal freezes rendered pixels during each upload and coalesces newer VT
changes for the next dirty rectangle. Initial display, configure changes, base
loss, and non-negotiating servers use full replacements. The single-cell and
adjacent-cursor fixture sends a 16x16 patch (1140 protocol bytes) and scans out
1024 bytes; distant dirty cells conservatively produce larger rectangles.

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

See [Terminal Configuration](terminal.md) for font sizes, color schemes,
custom palettes, and opacity settings.

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
7. Master bytes are parsed by the VT emulator and rendered to the terminal
   window surface without mirroring raw PTY output to the terminal client's
   own standard output.
8. Server-delivered key events are translated into escape sequences and written
   to the master.
9. Server-delivered window resizes send `TIOCSWINSZ` to the retained master and
   redraw the terminal grid.

## VT Emulator Scope

First version supports:

- cursor movement (`CUU`, `CUD`, `CUF`, `CUB`, `CUP`, `CHA`, `VPA`);
- erase operations (`ED`, `EL`, `ECH`);
- scroll regions (`CSR`) and screen scrolling, including explicit scroll-up (`SU`);
- delayed right-margin wrapping, canceled by cursor movement;
- SGR colors, bold, inverse, and underline;
- save/restore cursor (`DECSC`/`DECRC`);
- alternate screen buffer;
- basic UTF-8 display for printable characters;
- glyph rendering through the TTF cache and fallback font;
- tab stops;
- `OSC` title updates.

The terminal advertises `TERM=xterm-256color` and `COLORTERM=truecolor`.
`xterm-256color` is the ncurses 256-color profile derived from xterm and is
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

`CSI Ps SP q` selects the terminal cursor style: `0`/`1` blinking block
(the default), `2` steady block, `3` blinking underline, `4` steady underline,
`5` blinking bar, and `6` steady bar. `CSI ?12h`/`CSI ?12l` independently
enable/disable blinking without changing the shape; `RIS` restores the visible,
blinking block. Block cursors retain readable text beneath them. Blinking uses
500 ms monotonic deadlines, resets to the visible phase on keyboard input or
PTY output, and damages only the cursor cell. Hidden and steady cursors do not
schedule blink wakeups; pending cursor changes coalesce behind active uploads.
Application screenshot checks omit the live cursor cell; the dedicated cursor
fixture checks each shape and both blinking phases without assuming a fixed phase.

The packaged `/usr/share/vim/vimrc` sets Vim's terminal-mode sequences for
graphical terminals and nested tmux: Normal mode uses a blinking block,
Insert mode a blinking bar, and Replace mode a blinking underline. Leaving Vim
restores the terminal's default cursor. Cursor visibility changes preserve the
selected blink policy. These defaults require the graphical
`COLORTERM=truecolor` environment and do not affect text consoles. Starting Vim
with `-u NONE` bypasses the packaged configuration, including these sequences.
`terminal-vim-cursor.test` checks actual Vim mode changes and blinking, both
directly and inside tmux, using whole-cell shape comparisons.

Do not implement full xterm compatibility. The goal is enough ANSI/VT100
support for `oksh`, `vim`, `top`, `less`, and interactive Python.

Terminal behavior details:

- Keep 128 lines of scrollback per terminal window. Normal screen scrolling
  appends old lines to a ring buffer; alternate-screen scrolling does not add
  to scrollback. The wheel browses this history without moving the live cursor.
- Implement tab stops every eight columns, clear-tab, and set-tab behavior.
- Text selection and clipboard are deferred. The first terminal has no mouse
  selection or copy/paste because it requires a system clipboard service and
  more complete OSC 52 support. Keep `src/term.c` data structures ready for
  selection rectangles but do not ship the feature in the initial milestone.
- Report wheel steps to apps that enable `1000`, `1002`, or `1003` tracking,
  using SGR coordinates with `1006` or the legacy mouse encoding otherwise.
  Button clicks and motion reporting are not implemented.
- Resize behavior: the terminal computes the largest grid that fits the window
  minus scrollbar/chrome, then calls `TIOCSWINSZ` on the retained master with
  row/column counts. The primary grid is hard-reflowed when the column count
  changes; alternate-screen and soft-wrap reflow are deferred.
- Set the shell environment before exec: `TERM=xterm-256color`,
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

- run as a command-line application: `imageviewer /path/to/image`;
- decode BMP and PNG;
- fit the decoded frame to the window while preserving aspect ratio;
- receive normal window resize, minimize, maximize, focus, and close messages;
- use left/up and right/down arrow keys to switch to the previous or next
  BMP/PNG file in the same directory.

The decoder is selected by extension first and by magic bytes second; a
mismatch falls back to magic-byte detection. Decode failures print an error
and exit before opening a window.

Validation:

- A GUI test opens a known PNG/BMP, dumps the screen, and checks that the
  image is visible.

### App Launcher and System Panel

Add a simple launcher to the desktop panel:

- terminal;
- about box;
- reboot/halt if the kernel exposes the existing reboot path to userspace.

The launcher is a grid of buttons above the panel. It sends typed launch
requests to `mario-wm`; the server starts only the approved initial client
binaries and reaps them. A shell child never inherits raw-seat, listener, or
shell-capability fds. The image viewer is launched directly from a terminal
prompt, not from the panel.

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
  no framebuffer, raw-input, listener, or unrelated client fd, and launched
  ordinary applications bind standard streams to `/dev/null` rather than
  inheriting `/dev/console` or `/tmp/mario-wm.log`.
- Desktop appearance uses `$HOME/.config/mario/desktop.conf` (or the XDG/explicit
  override) for font selection and shared colors. See [Desktop Appearance](appearance.md).
  Session commands, panel placement, and application paths remain compiled-in;
  the proposed `/etc/mario-gui.conf` session-settings loader is not implemented.
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
app/usr/launcher/
app/usr/Makefile
app/usr/freetype/
app/usr/libpng/
app/gui/libmario-ui/assets/fonts/  # bundled TTF source files
rd/_ramdisk0/root/.config/mario/desktop.conf  # bundled desktop appearance defaults
rd/_ramdisk0/root/.config/mario/terminal.conf # bundled terminal defaults
rd/device_list             # adds fb0, gui, psaux, ptmx, pts/0..31
```

Test:

```text
tests/gui/fb.test
tests/gui/gui-desktop.test
tests/gui/gui-terminal.test
tests/gui/imageviewer-bmp.test
tests/gui/imageviewer-png.test
tests/gui/gui-font.test
tests/shell/pty.test
tests/shell/mouse.test     # read-only packet smoke test if device present
```
