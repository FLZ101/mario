# Window Management and Launcher Plan

Status: proposed follow-up to M10.  This document is a detailed target
design.  The implementation roadmap remains the authority for milestone
ordering and commit boundaries; fold these items into the roadmap before
starting implementation.

## Goal

Make normal windows first-class desktop citizens:

- move windows by their title bar;
- resize every edge and corner with the mouse;
- minimize, maximize/restore, and close through visible title-bar controls;
- expose a useful desktop menu/launcher;
- start every GUI application from the desktop rather than from a TTY.

## Current State

The existing `mario-wm` already has the core pointer mechanics:

- title-bar move;
- left/right/top/bottom and four-corner pointer resize with clamping and
  configure/surface transaction handling;
- a click target in the top-right title-bar area that sends
  `WINDOW_CLOSE_REQUEST`;
- taskbar window buttons and `SHELL_ACTIVATE_WINDOW`.

What is missing or unfinished:

- the close control is a solid dark rectangle, not an icon;
- there are no minimize/maximize controls or window states;
- resize hit zones are one-pixel edges/corners with no visible affordance;
- the panel `MENU` launcher currently only raises/creates the two diagnostic
  `Welcome` and `Display` windows, not real applications;
- real applications are normally launched from tests or from a TTY shell,
  even though `mario-wm` already implements `LAUNCH_REQUEST` and can fork
  `terminal`.

## Design Decisions

- Keep window state owned by `mario-wm`; clients receive authoritative
  configure/state notifications but never choose their own minimized state.
- Reuse the existing configure/surface transaction machinery for maximize.
  Minimize hides a window without changing its committed surface or requiring
  a client redraw.
- Maximize means "fill the shell work area", not fullscreen.  Fullscreen
  remains a separate mode and continues to hide the panel/chrome.
- Add a shell-reported work area so the window manager does not duplicate the
  panel theme constants.  The shell sends its panel geometry after
  `DESKTOP_CREATE`.
- Keep the existing title-bar theme and make control icons deterministic,
  reviewed pixels rather than a new image/icon subsystem.
- Do not add window contents with a new toolkit.  The menu is part of the
  shell surface and uses existing `libmario-ui` drawing/fonts.

## Non-Goals

- Drag-to-edge tiling/snapping;
- keyboard-only resize;
- minimize animations or live window thumbnails;
- multiple workspaces/virtual desktops;
- per-application icons or a desktop icon grid;
- clipboard, drag-and-drop, or file open/save dialogs;
- replacing the cursor sprite with multiple resize cursors in the first pass.

## Work Items

### M11.1 — window states and shell work area

Prerequisite: M10.  Owns `app/gui/libmario-ui/include/gui/protocol.h`,
`app/gui/libmario-ui/src/protocol.c`, `app/gui/desktop/mario-wm/main.c`,
`app/gui/desktop/mario-desktop/main.c`, `app/test/gui-wm-client/`,
`tests/shell/gui-wm-busy.test`, and `tests/gui/wm-window-state.test`.

Add:

```c
GUI_PROTOCOL_WINDOW_STATE,
GUI_PROTOCOL_SHELL_WORK_AREA,
```

and packed wire messages:

```c
typedef struct __attribute__((packed)) {
    uint32_t window_id_le;
    uint32_t state_le;
    uint64_t configure_serial_le;
} gui_protocol_wire_window_state;

typedef struct __attribute__((packed)) {
    int32_t x_le;
    int32_t y_le;
    uint32_t width_le;
    uint32_t height_le;
} gui_protocol_wire_shell_work_area;
```

The new enum values follow `GUI_PROTOCOL_SURFACE_CANCEL`.  Update
`gui_protocol_type_valid()` in `protocol.c` to accept through the new last
message type instead of hard-coding `GUI_PROTOCOL_SURFACE_CANCEL`.

Window states:

```c
GUI_PROTOCOL_WINDOW_STATE_NORMAL,
GUI_PROTOCOL_WINDOW_STATE_MINIMIZED,
GUI_PROTOCOL_WINDOW_STATE_MAXIMIZED,
```

WM behavior:

- `mario-wm` tracks `minimized` and `maximized` separately from `fullscreen`.
- Minimize cancels capture, resets input ownership, marks the window hidden
  from composition, and notifies the shell taskbar.  It sends
  `WINDOW_STATE` but no `WINDOW_CONFIGURE` and does not invalidate the
  committed surface.
- Restoring a minimized window reveals the existing committed surface,
  focuses/raises it, and sends `WINDOW_STATE` normal.
- Maximize saves normal geometry, transitions to the shell work area through
  the existing configure + surface reservation path, and publishes
  `WINDOW_STATE` maximized after the matching commit.
- Restoring maximized windows uses the same transaction path back to normal
  geometry.
- Fullscreen remains exclusive; minimize/maximize requests are rejected while
  fullscreen or mode/transition pending.

Shell behavior:

- After `DESKTOP_CREATE`, the shell sends one `SHELL_WORK_AREA` for the desktop
  area excluding the panel.  `mario-wm` accepts it only from the private shell
  capability and clamps maximize geometry to it.
- The private shell uses `SHELL_SURFACE_COMMIT` to commit a desktop frame with
  its launcher popup rectangle (zero size when closed). The WM draws that
  rectangle above application windows and routes pointer hits inside it to
  the shell. The cursor remains the topmost layer.
- After the initial desktop upload, opening or closing the popup and hovering
  over its rows patch only the popup rectangle and take priority over pending
  taskbar redraws. The row under the pointer stays highlighted even when an
  application owns focus. Closing the menu restores the popup's background
  without uploading the rest of the wallpaper.
- Hovering over the panel's MENU button highlights it without changing focus.
  Entering or leaving the button patches only the panel.
- The menu launches Terminal, File Manager, and System Monitor directly, with
  Reboot and Shutdown below them.

Success: minimize/restore preserves exact pixels and client state; maximize
reaches exact work-area geometry and restore reaches exact prior geometry.
Failure: insufficient surface capacity, stale configure, or commit/flush error
restores the previous mode and leaves the last committed surface visible.

Focused gates:

```sh
.venv/bin/python tests/lit_shell_test.py --smp 16 --test gui-wm-busy.test tests/shell
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-window-state.test tests/gui
```

Commit: `gui: add window states and shell work area`.

### M11.2 — visible title-bar controls and hit targets

Prerequisite: M11.1.  Owns `app/gui/desktop/mario-wm/main.c`,
`tests/gui/wm-chrome.test`, reviewed assets under `tests/gui/assets/`, and
this document.

Replace the solid close rectangle with three deterministic controls packed from
the right edge:

- minimize (`_`);
- maximize (`[+]`) or restore (`[-]`) depending on state;
- close (`x`).

Each control is 16 px wide by `WM_TITLE_HEIGHT` high.  With the existing
`WM_MIN_CONTENT_WIDTH` of 64 px, all three controls fit and leave a 16 px
title-drag strip at minimum width.

Requirements:

- Render active/inactive/hover/pressed states from the existing theme palette.
- Keep the remaining title area a drag-only strip.  The current WM does not
  draw window title text, so this item does not add a font dependency.
- Hit-test exactly the visible control rectangles; title drag occupies only
  the space between the controls and the content border.
- Close sends `WINDOW_CLOSE_REQUEST`; minimize/maximize call the M11.1 state
  transitions.
- Disable/hide minimize/maximize chrome while fullscreen.

Success: QMP crops show exact icons in normal, hover, pressed, active, and
inactive states, and clicks perform the expected action.
Failure: malformed or missing control pixels do not pass; actions must not
send input events into the client content.

Focused gate:

```sh
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-chrome.test tests/gui
```

Commit: `gui: draw title-bar controls and hit targets`.

### M11.3 — resize affordances and transaction hardening

Prerequisite: M11.2.  Owns `app/gui/desktop/mario-wm/main.c`,
`tests/gui/wm-interact.test`, `tests/gui/wm-resize.test`, and reviewed assets.

The pointer machinery exists; make it visible and verifiable:

- Replace the exact one-pixel boundary test with a shared `WM_RESIZE_GRIP`
  band (for example 4 px) for each edge/corner, and draw the matching
  corner/edge affordance when the pointer is in that band or while a resize
  capture is active.
- Keep the actual hit region and drawn affordance in one shared geometry
  helper so they cannot drift.
- Preserve minimum/maximum clamping, work-area bounds, configure serial
  increments, input resets, and rollback on allocation/flush failure.
- Do not deliver resize-drag input to the client surface.
- Add exact tests for every edge/corner, an immediate release, a zero-motion
  click, and minimum-size saturation.  Update `wm-interact.test` coordinates
  if the larger grip band changes the required starting points.

Success: visible affordance matches hit region; all eight resize directions
produce exact final geometry and pixels.
Failure: any partial configure, leaked capture, or client-input leak fails.

Focused gates:

```sh
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-interact.test tests/gui
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-resize.test tests/gui
```

Commit: `gui: harden pointer resize affordances`.

### M11.4 — desktop application menu

Prerequisite: M11.1.  Owns `app/gui/desktop/mario-desktop/main.c`,
`app/gui/desktop/mario-wm/main.c` only for launch-result validation,
`tests/gui/gui-launcher.test`, and reviewed assets.

Replace the diagnostic launcher with a real shell-owned popup menu:

- `MENU` in the existing panel slot opens the menu above the panel.
- Menu entries use the existing protocol and WM launch implementation:
  - `Terminal`: `LAUNCH_REQUEST` type `LAUNCH_TERMINAL`;
  - The minimal image viewer is intentionally launched from a terminal:
    `imageviewer /path/to/image`.
- Menu opens/closes with mouse and Escape, restores focus after activation,
  and closes when a launch is accepted.
- Request IDs are monotonic and per-shell.  The shell consumes
  `LAUNCH_RESULT`; a negative result remains visible in the panel/menu without
  disconnecting the shell.
- The WM continues to validate path bytes, launch type, and shell role before
  forking; no application inherits the WM seat descriptors.

Success: clicking `Terminal` starts a real GUI window; the menu closes; the
taskbar button appears; error results are reported.
Failure: launch from a non-shell peer remains rejected; malformed paths do
not fork; no app is started on a text VT.

Focused gate:

```sh
.venv/bin/python tests/lit_gui_test.py --smp 16 --test gui-launcher.test tests/gui
```

Commit: `desktop: add application menu and launcher`.

### M11.5 — desktop-only application startup

Prerequisite: M11.4.  Owns `app/gui/desktop/mario-wm/main.c`,
`app/gui/desktop/mario-desktop/main.c`, `tests/gui/wm-autostart.test`,
`tests/gui/wm-startup.test`, `tests/gui/wm-launch.test`, and this document.

Make the desktop the normal application entry point:

- Keep the current init path: `mario-wm` starts, creates the private shell
  socketpair, and forks `mario-desktop`.
- Remove the diagnostic `Welcome`/`Display` windows from normal startup; keep
  them only behind an explicit `--diagnostics` shell test mode if still needed.
- The shell starts with the desktop, panel, taskbar, and application menu
  ready; it does not rely on a TTY-launched application.
- Update launch tests so `terminal` is exercised through the desktop menu and
  `imageviewer` is exercised from a command line.
- Preserve shell replacement/recovery: a new shell sends its work area and
  rebuilds the menu/taskbar from a fresh snapshot.
- Keep `mario-wm --` and TTY fallback behavior for diagnostics, but no normal
  GUI app launch path depends on it.

Success: a fresh boot plus `F8` presents the desktop menu; launching every
installed GUI app creates a normal managed window and clean reclamation.
Failure: shell crash recovery still recreates the menu without stale launch
records; a TTY-only launch path is no longer required for the GUI suite.

Focused gates:

```sh
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-autostart.test tests/gui
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-startup.test tests/gui
.venv/bin/python tests/lit_gui_test.py --smp 16 --test wm-launch.test tests/gui
```

Commit: `desktop: start applications through the launcher`.

## Validation

After each item:

```sh
make test SMP=16
git diff --check
```

Visual tests must use reviewed QMP screenshots and `GUI-WAIT-FRAME`, not fixed
sleeps.  Existing fullscreen, taskbar, launch, terminal, file-manager, and
recovery fixtures remain unchanged except where a test intentionally adopts
the new menu or window-state behavior.

## Risks and Rollback

- **Theme duplication:** the work-area message avoids duplicating panel height
  in `mario-wm`; protocol changes are isolated to M11.1.
- **Client compatibility:** new `WINDOW_STATE` and `SHELL_WORK_AREA` messages
  are optional for v1.0 clients and must not break existing v1.0/v1.1 tests.
- **Fullscreen interaction:** fullscreen already hides chrome and owns the
  whole screen; minimize/maximize must not weaken that contract.
- **Golden churn:** title-bar icons and resize affordances change pixels.
  Existing goldens must be reviewed and updated only in the commit that adds
  the feature.
- **Launch recovery:** failed `exec`/`fork` must leave the requesting shell
  alive and report a negative `LAUNCH_RESULT`; it must not require WM restart.
- **TTY fallback:** retain text VTs and serial diagnostics as fallback, but do
  not reintroduce a normal GUI-launch path that bypasses `mario-wm`.
