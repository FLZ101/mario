# FLTK port for Mario

## Scope

Make it possible to rebuild existing FLTK applications for Mario, not just
write apps against a new Mario-specific widget API. Start with FLTK 1.4.5 and
a native software backend. An application should use ordinary FLTK headers
and `Fl::run()`; the backend should connect as an ordinary `mario-wm` client.
The window manager keeps ownership of raw input, the framebuffer, compositing,
and decorations. This is a source port, not Linux binary compatibility.

Prove the backend with an untouched upstream FLTK editor, then ship two
Mario-native FLTK apps: a full desktop File Manager and a graphical System
Monitor. They use stock FLTK widgets with separate filesystem and telemetry
models. Keep Dillo as an optional local-HTML compatibility port; it does not
block either app. No dependency on LVGL, X11, Wayland, OpenGL, or a new
desktop shell. Do not require kernel changes for the toolkit or first app
releases.

## Milestones

| Milestone | Requires | Deliverable |
| --- | --- | --- |
| F1 — Cross-build | Existing Mario C++ toolchain | Static FLTK core and driver scaffold |
| F2 — Offscreen drawing | F1 | Software rendering verified without a WM connection |
| F3 — Window lifecycle | F2 | Native windows, configure handling, and surface uploads |
| F4 — Event loop and input | F3 | Interactive demo running through `Fl::run()` |
| F5 — Upstream compatibility | F4 | Unmodified FLTK editor exercised end to end |
| F6 — File Manager browsing | F5 | Tabs, split view, bookmarks, and context menus |
| F7 — Copy jobs | F6 | Cancellable, no-clobber copy with progress |
| F8 — Move and recovery | F7 | Create, rename, move, trash, restore, and permanent delete |
| F9 — Telemetry model | Existing Mario `/proc` and C++ toolchain | Tested CPU, memory, task, and mount snapshots |
| F10 — System Monitor UI | F5, F9 | Live, read-only monitor with graphs and tables |
| F11 — Packaging | F8, F10 | Both applications installed in standard and GTK images |
| F12 — Desktop integration | F11 | Menu launch, lifecycle checks, and release validation |
| D1 — Optional Dillo port | F5 | Offline local-HTML compatibility test |

Keep each milestone buildable and testable with its prerequisites. F9 can
start immediately without FLTK or a working WM. After F5, File Manager and
System Monitor work can proceed independently. Probes live in
`app/test-cxx/`; installable app sources and recipes live in `app/usr-cxx/`.
The test paths below are proposed, not a status report.

### F1 — Static build and driver selection

1. Pin FLTK `release-1.4.5` from
   `https://codeload.github.com/fltk/fltk/tar.gz/refs/tags/release-1.4.5` with
   SHA-256 `7715e69ce081fa9ce6da48bb0dd3b07a4cf2cf937813814c04272f36fff593ea`.
   Verify fresh and cached archives before extraction. Keep `fltk.mk`,
   `build-fltk.sh`, platform patches, and build/staging outputs under
   `app/usr-cxx/`.
2. Build after libc++ with `mario-musl-c++` and `mario-musl-cc`. Use the
   existing CMake toolchain pattern and resolve Ninja from `CXX_PYTHON`'s
   venv. Rebuild on toolchain, patch, or config changes. Enable
   `COMPILE_COMMANDS_JSON` for native code, not upstream sources.
3. Add a distinct Mario backend and wire FLTK's system, screen, window,
   graphics, and surface-driver factories. Reuse portable POSIX code where
   supported and keep platform changes out of widgets. Disable shared
   libraries, other backends, OpenGL, and guest FLUID. Don't run target
   binaries or discover host headers and libraries during configuration.
   If FLUID is needed later, build and run it as a separate host tool.
4. Stage the core archive, public headers, generated config headers, and
   target link requirements. Record the FLTK license and static-linking
   exception; preserve notices and corresponding source for distributed
   FLTK changes. Add a link probe in `app/test-cxx/fltk-probe/`.

**Exit gate:** `readelf` shows ELF32/i386 with no `INTERP` or `NEEDED`
entries; the probe constructs stock FLTK widgets in
`tests/shell/fltk-build.test` without a WM connection or host GUI libraries.

The guest image carries FLTK's unchanged `COPYING` at
`/usr/share/doc/fltk/COPYING`. To reproduce or redistribute the modified
FLTK source, use the pinned archive together with `app/usr-cxx/fltk.patch`
and the Mario driver sources in `app/usr-cxx/fltk/`.

### F2 — Offscreen software drawing

1. Connect FLTK's offscreen surface driver to `libmario-ui` pixel buffers.
   Define the pixel format, stride, origin, and clip behavior once for both
   offscreen drawing and F3's windows. Set allocation limits and reject
   dimensions that overflow buffer size calculations.
2. Implement the clip stack, fills, lines, paths, and RGB/RGBA blits. Use
   premultiplied `ARGB8888` for the client surface and convert source alpha
   at the blit boundary. Restore the prior surface, origin, and clip when
   returning from a nested offscreen operation.
3. Use `libmario-ui`'s FreeType-backed fonts with a deterministic bundled
   mapping for FLTK font families. Share glyph advances between measurement
   and rendering. Audit remaining operations against the editor and stock
   widgets before adding a raster dependency.

**Exit gate:** `tests/shell/fltk-render.test` checks exact pixels for nested
clipping at a nonzero offset, fills, color and alpha, and image blits, plus
glyph placement and width for a pinned font. The probe never opens a window.

### F3 — WM lifecycle and uploads

1. Connect as an ordinary client to `/tmp/mario-wm.sock`. Reuse `libmario-ui`
   wire and upload APIs via C++-compatible headers or a narrow C bridge;
   don't duplicate wire structs. Track the WM window ID and accepted
   configure serial. Implement show, hide, title, resize, and close, with
   state kept per window even if the first probe shows only one.
2. Treat WM configure as authoritative: resize the client surface and redraw
   without issuing a second resize request. Use F2's renderer for
   `Fl_Window` and `Fl_Double_Window`. Submit a full frame on create or
   configure before attempting any damage optimization.
3. Use `gui_surface_upload` and negotiate patches before sending damage.
   Associate each sent base with its configure serial; a written commit
   doesn't prove presentation. Invalidate a rejected base and recover with
   a full frame. Keep one immutable upload snapshot per window, coalesce
   new damage separately, and bound allocation across windows. Drain or
   cancel obsolete work without interleaving a partial wire record; discard
   stale replies and free window and upload state on disconnect.

**Exit gate:** `tests/shell/fltk-upload.test` uses a fake socket peer for
short writes, delayed replies, and rejected patch bases.
`tests/gui/fltk-window.test` verifies visible create, resize during an upload,
damage that leaves neighboring pixels unchanged, and close teardown.

### F4 — Input and event loop

1. Integrate the nonblocking WM socket and `Fl::add_fd()` callbacks into
   FLTK's platform wait. Poll until a descriptor is ready or the next
   monotonic timer deadline. Bound socket work per iteration so sustained
   input and writes don't starve timers or other descriptors. Don't dispatch
   events from drawing callbacks.
2. Translate window-local pointer coordinates, button changes, drag, wheel,
   focus, and modifier state. Deliver wheel events to the WM's wheel target,
   not the keyboard-focus window. Cancel held and grabbed state on
   `INPUT_RESET` or focus loss without generating a click.
3. Map key presses to navigation, shortcuts, and text using Mario's current
   key map. Don't advertise physical key-up semantics that the WM doesn't
   provide. Route WM close through the FLTK close callback so an app can
   decline it; verify `Fl::run()` returns after the final window closes.

**Exit gate:** `tests/gui/fltk-demo.test` exercises a button, text field,
scrollable area, timer, and pipe callback. It verifies focus handoff to a
terminal, no click after focus loss during a press, and clean exit. Confirm
an idle window blocks instead of spinning, and callbacks run during traffic.

### F5 — Upstream editor compatibility

1. Build FLTK's upstream `test/editor.cxx` as a guest fixture against the
   staged library without adding Mario calls or patching the example.
   Exercise menus, modal file choosers, multiple windows, double-buffered
   redraw, and in-process text copy/paste. Defer cross-app clipboard, IME,
   drag-and-drop, printing, and HiDPI.
2. Start with toolkit-local modality and client-local popups where possible.
   Multiple ordinary WM windows alone don't establish transient stacking,
   dismissal, or grabs. If the editor needs a behavior the WM can't express,
   document the failing case and review a narrow userspace protocol
   extension. Don't use the privileged desktop endpoint for client menus.
3. Exercise keyboard and pointer menu navigation, chooser cancellation,
   focus return, and closing one editor window without closing the other.

**Exit gate:** `tests/gui/fltk-editor.test` opens a fixture with spaces in
its path through the stock chooser, edits it, and saves through the stock
menu. Check the visible edit and exact saved bytes, then close all windows.

### F6 — File Manager: browsing and context menus

1. Add `app/usr-cxx/filemanager/` and the `/usr/bin/filemanager [directory]`
   interface. Build a directory model around `opendir`/`readdir` without
   depending on `d_type` or `inotify`. Preserve raw filename bytes separately
   from display labels; handle vanished entries and never truncate paths.
   Read in bounded batches with a visible 4,096-entry limit per listing.
   Cancel obsolete scans and discard late results after navigation.
2. Add back/forward/up/home, an editable location bar, independently
   navigable tabs, optional dual panes, and a sidebar with bookmarks and a VFS
   directory tree rooted at `/`. Load branches on expansion; mount points
   appear in place, and directory symlinks are not expanded. Use `Fl_Tabs`,
   `Fl_Tree`, and `Fl_Table_Row` and draw only visible rows. Add sortable
   name/type/size/modified columns, multiselect, a hidden-file toggle, filename
   filtering, shortcuts, and properties.
3. Add explicit refresh and persist bookmarks and view preferences in
   `~/.config/mario/filemanager.conf`. A bad or unwritable config must not
   prevent browsing. Show broken links and require an explicit action to
   follow a directory symlink.
4. Add context menus for selected rows and empty directory space. Keep the
   current multiselection when right-clicking a selected row. Open BMP and PNG
   files in `/usr/bin/imageviewer` and MP4 files in `/usr/bin/mpv`. Pass the
   absolute path as a separate argument. Close GUI descriptors on exec, report
   exec failures, and reap viewer children without blocking the UI.

**Exit gate:** `tests/shell/fltk-filemanager-model.test` covers ordering,
filters, limits, names with spaces and invalid display bytes, and disappearing
entries on MarioFS, ext2, and VFAT (including read-only mounts).
`tests/gui/fltk-filemanager.test` verifies navigation, independent tabs and
panes, bookmark persistence, right-click actions, and visible image-viewer
launch and teardown.

The browser lists up to 4,096 entries per pane and sorts only the loaded
entries. Double-click a row or use Ctrl+Enter to open it; Ctrl+L edits the
location, Ctrl+R refreshes, Ctrl+T opens a tab, and Ctrl+D toggles a bookmark.
Use the View menu to show hidden files or split the current view. Bookmarks
encode raw path bytes in hex in the settings file. Right-click a row for file
actions or click empty space with the right button for directory actions.
The Image Viewer opens separately and can be closed independently.

### F7 — File Manager: copy jobs

1. Probe existing filesystem primitives on each guest filesystem. Mario has
   `openat`, `mkdirat`, `renameat`, and `unlinkat`, but not `renameat2` or a
   native `fstatat64`; verify libc fallbacks before relying on them. Build a
   model for selected sources, destination, work still pending, and outcome
   per item so skipped or failed entries aren't counted as success.
2. Implement recursive copy with exclusive file and directory creation.
   Offer Skip or Keep Both, never overwrite; reserve alternate names using
   the same exclusive operations. Refuse destinations inside the source and
   do not follow links or cross nested mounts during traversal. Preserve
   symlinks as links where supported and report unsupported entries and
   metadata rather than silently changing their meaning.
3. Run one mutation per browser in a bounded helper process. Pass progress,
   conflict decisions, and results via a protocol serviced through
   `Fl::add_fd()` without blocking the UI. Check for cancellation between
   chunks and clean only job-owned partial output. Offer wait or cancel when
   closing, then reap the helper and remove its fd callbacks.
4. Add multiselect copy/paste using an app-local list, progress and cancel
   controls, and a completion summary. Refresh affected panes when the job
   completes. Limit memory use without requiring a complete recursive listing
   or an exact total byte count before starting.

**Exit gate:** `tests/shell/fltk-filemanager-copy.test` checks exact bytes,
preservation of existing items, cancellation at a controlled chunk boundary,
symlinks, and read-only/full destinations on the guest filesystem matrix.
`tests/gui/fltk-filemanager-copy.test` copies between panes, handles a name
conflict, and verifies output and worker teardown over serial.

Use **Edit → Copy** (`Ctrl+C`) and **Edit → Paste** (`Ctrl+V`) with a pane
active. The status strip reports progress, offers Skip or Keep Both for a
conflict, and lets you cancel. **Edit → Last Job Summary** lists failures
and unsupported items. Copy preserves file bytes and supported symlinks,
not metadata or hard-link relationships. Directories never merge. The
guest supports relative `fstatat` through `statx`; rollback uses
`fchdir`/`rmdir` because `unlinkat` does not implement `AT_REMOVEDIR`.

Each selected item is copied independently. Cancelling or failing an item
rolls back its journal of created entries without scanning for other files
to remove. Rollback checks inode identities, but is not atomic against
external changes to the destination. If cleanup fails, the summary flags
possible partial output. Copies use 32 KiB chunks and limit each selected
tree to 48 levels and 16,384 created entries.

### F8 — File Manager: move, trash, and recovery

1. Add new folder, rename, and cut/paste on the F7 job model. Do not use an
   existence check followed by `renameat` as no-replace. Where a true
   no-replace move is unavailable, use copy-then-remove for supported
   entries or report the limitation, including for same-volume moves.
2. Remove a move source only after its entire destination copy succeeds
   and its source entries are rechecked. Any copy failure, skipped
   descendant, or cancellation before removal retains the source item.
   If removal later fails, keep the completed destination and report which
   source entries remain. Don't promise crash-atomic moves or metadata the
   destination cannot represent.
3. Implement per-volume trash using an app-owned location, exclusive item
   slots, and original-path metadata. Define how incomplete slots are
   reported and recovered before enabling Trash. Restore with the same
   no-clobber policy. If trash is unavailable, report that rather than
   silently converting the operation to permanent deletion.
4. Keep permanent delete separate and require explicit confirmation with a
   selection summary. Show destinations for move and restore, protect roots
   and mount points, and reuse F7's traversal boundaries and job lifecycle.

**Exit gate:** `tests/shell/fltk-filemanager-move.test` verifies successful
cross-volume moves and source retention on failed, cancelled, or partially
skipped copies, plus rename conflicts, trash/restore, and protected paths.
`tests/gui/fltk-filemanager-mutations.test` trashes and restores an item and
declines a permanent-delete confirmation; serial checks verify paths and
bytes after each operation.

Use **File → New Folder** (`Ctrl+Shift+N`) or **Rename** (`F2`), or cut
with `Ctrl+X` and paste into a different folder with `Ctrl+V`. Mario uses
copy-then-remove, even on one volume: before removing anything, the worker
rechecks the recorded trees and compares file bytes and symlink targets.
Cancel before removal to retain the current source item. Once removal
starts, the worker finishes that item or stops at its first error, keeps the
complete destination, and reports where it stopped and how many recorded
entries it did not remove. These are not atomic operations against crashes
or concurrent external filesystem changes.

`Delete` moves selected entries to `.mario-trash` at the root of their
volume. The File Manager checks the store's owner marker and never falls
back to permanent deletion if it cannot use the store. **Go → Volume Trash**
(`Ctrl+Shift+T`) opens it. Select a slot and use **Edit → Restore or Recover
Selected** (`Ctrl+Shift+R`) to see its original path. Ready slots restore
without overwriting. Incomplete slots may contain partial data; recover a
copy into a folder you choose and compare it with the original or completed
destination before discarding anything. Restoration clears the ready marker
before removing the saved payload, so an interrupted restore cannot offer
its remaining payload as a complete item. `Shift+Delete` permanently
deletes a selection only after confirmation; use it on a Trash slot to
discard it, including incomplete slots with no payload.

### F9 — System Monitor: telemetry model

1. Build an FLTK-independent collector under `app/usr-cxx/system-monitor/`.
   Reuse the Mario `/proc` knowledge from the `btop` port, not its terminal
   UI or output. Add an injectable source for deterministic snapshots and
   keep live collection and parsing separate from rate calculations.
2. Collect the fields below with bounded reads and enumeration. Keep valid
   rows when individual tasks disappear or mounts fail. Represent missing,
   invalid, or truncated data explicitly rather than substituting zero.

   | Source | Display and limits |
   | --- | --- |
   | `/proc/stat` | Aggregate and online CPU rows. Compute busy share from user/system/idle deltas; initial samples, resets, and zero-total intervals have no rate. |
   | `/proc/meminfo`, `/proc/uptime` | Used memory is total minus free; show cache separately and use the first uptime field. Do not infer Linux reclaim or swap data. |
   | `/proc/<pid>/stat`, `/proc/<pid>/comm` | PID, parent, task name/state, and CPU ticks. Key identity and deltas by PID plus start ticks; label CPU as a share of total machine time. Do not rely on placeholder RSS/thread fields or treat `cmdline` as a full argument list. |
   | `/proc/mounts`, `statvfs` | Mount, filesystem, capacity, and available space. Parse known prefix and suffix fields without splitting an unescaped path on spaces. Report unmount races and `statvfs` failures per mount. |

3. Record monotonic sample times and derive rates only across valid pairs.
   Treat PID reuse as a new row and don't carry rates across counter resets
   or stale collection gaps. Leave out per-task memory, load average, and
   disk/network throughput until the corresponding data is available.

**Exit gate:** `tests/shell/fltk-system-monitor-model.test` asserts exact CPU
deltas, first/zero/reset samples, task names containing parentheses, task
exit and PID reuse, malformed fields, mount paths with spaces, and a
failing mount. Live guest snapshots show the expected per-core rows at
`SMP=1` and `SMP=2` without assuming exact QEMU percentages. No WM is needed.

### F10 — System Monitor: live FLTK UI

1. Build `/usr/bin/system-monitor` with Overview, Processes, and Filesystems
   tabs. Add aggregate and per-core CPU graphs, memory/cache history and
   uptime, a sortable and filterable task table with details, and mount
   capacity bars. Keep task selection by identity as the table reorders;
   don't move it to a new process that reuses the selected PID.
   The Processes tab's Tree toggle groups children under their parents.
   Click a branch arrow to fold it; sorting reorders siblings, and filtering
   shows matching tasks with their ancestors without changing folded state.
2. Schedule one sample per second with FLTK timers and a 120-sample ring.
   Collect outside painting and keep I/O off the UI path; allow one refresh
   in flight and skip ticks while it runs. Add pause and manual refresh,
   mark unavailable or stale data, and redraw only changed regions. On
   close, unregister timers and fd callbacks and stop and reap the worker.
   Pause/Resume, Refresh, and Tree use glyphs from the bundled Nerd Font
   alongside their text labels.
3. Select a process and choose **End Process…** (`SIGTERM`) or **Force
   Kill…** (`SIGKILL`). Both show an icon and confirm the name and PID, with
   Cancel as the default. Init, the monitor and its sampler, individual
   thread rows, and exited processes cannot be targeted from the UI.
   Mario's `prctl(MARIO_PR_SIGNAL_PROCESS, pid, start_ticks, signal, 0)`
   checks the PID, start time, and process leadership under the task-list
   lock before delivering either signal to the leader. A default fatal
   disposition stops the thread group, not child processes in the tree.

**Exit gate:** `tests/gui/fltk-system-monitor.test` uses a test-only snapshot
source to verify graph pixels, sorting, and selection across refreshes.
`tests/gui/fltk-system-monitor-tree.test` covers hierarchy, branch folding,
filtering, parent PID reuse, and control icon rendering with a deterministic
snapshot.
`tests/gui/fltk-system-monitor-live.test` checks a guest workload appearing,
accumulating CPU time, and exiting, plus refresh after a mount change. Check
per-core rows at both SMP settings and verify pause and close stop sampling.
The `fltk-system-monitor-signals.test` shell and GUI fixtures verify checked
identity, thread-group teardown, both buttons, and cancel-by-default.

**Shared font icons:** FLTK apps can include `app/usr-cxx/font-icons.mk`
instead of `fltk-app.mk`. `mario::IconFont` loads the bundled Nerd Font by
default or accepts another font path, pixel height, and icon cell size.
Call `render(codepoint, rgb)` to create an owned image; keep that image alive
while widgets use it with `mario::set_label_icon`. A missing font or glyph
leaves the label text visible without an icon. File Manager and System
Monitor render their icon sets once at startup and reuse the images.

### F11 — Production packaging

1. Wire FLTK and both apps into the normal C++ build with explicit FLTK
   and `libmario-ui` prerequisites for the app binaries. Keep the editor,
   probes, and test snapshot source in test builds, not production apps.
2. Stage executables, required assets, and notices through `rd/copy_files.py`
   for both standard and GTK images. Do not install headers, archives,
   or host build tools. Verify that image overlays do not hide required
   assets or install a second app at either binary path.
3. Boot rebuilt images and invoke both binaries directly before changing
   the launcher. Verify stock fonts and usable defaults without a preexisting
   settings file.

**Exit gate:** both images contain the two production binaries and required
assets; both apps open and close when invoked by their installed paths.
`tests/shell/fltk-install.test` checks the standard image's installed files;
apply the same checks to the GTK image before F12.

### F12 — Desktop integration and release validation

1. Add File Manager and System Monitor menu entries. Reuse the reserved
   File Manager selector and add a narrow System Monitor selector. Accept
   fixed-app, zero-path requests only on the private desktop endpoint,
   using the existing WM child reaper. Launch `/usr/bin/filemanager` from
   the single default File Manager menu entry.
2. Add `tests/gui/fltk-launcher.test` for menu launch, repeat launch/close,
   and worker and window teardown. Extend `tests/gui/wm-launch.test` to
   reject both launch requests from ordinary clients with and without a
   window, and verify no new application process or window appears.
3. Run focused fixtures at 1024×768 with the standard 256 MiB guest, then
   run affected launcher, WM, terminal, image-viewer, and `btop` regressions.
   Manually verify menu launch, menus and dialogs, pointer and wheel,
   maximize/restore at 1920×1080, and repeated launch/close in
   `make run-gtk`. Record image size, peak client memory, idle CPU, and
   process/window counts before launch and after teardown.

**Exit gate:** menu launch and close work in both images. The focused suite
passes at `SMP=1` and `SMP=2`, existing regressions stay green, and repeated
launch/close leaves no app workers or WM windows behind.

### D1 — Optional Dillo compatibility port

1. Use Dillo 3.3.0 as the initial candidate; upstream FLTK 1.4 support is
   experimental and requires `--enable-experimental-fltk`. Pin and verify
   the source archive. Inventory platform calls, required image libraries,
   and helper processes before selecting a local-only configuration.
2. Cross-build against the staged FLTK and existing guest libpng and zlib
   where needed. Add dependencies only if the local-file build needs them.
   Make unsupported network paths explicit; leave DNS, HTTP(S), TLS
   certificates, and optional helpers for follow-up work.
3. Add an offline `file://` fixture with styled text, an inline PNG, and a
   relative link to a second page. Keep it out of the two apps' release gate.

**Exit gate:** `tests/gui/fltk-dillo.test` opens the local fixture, follows a
relative link, navigates back, scrolls, resizes, and closes cleanly.

## Running Milestone Tests

After rebuilding guest images with the required fixture binaries, run the
focused tests for the current milestone. For example, once F2 and F5 land:

```sh
make -f Makefile.run.mk shell-test SHELL_TEST_CASES=tests/shell/fltk-render.test SMP=1
make -f Makefile.run.mk gui-test GUI_TEST_CASES=tests/gui/fltk-editor.test SMP=1
```

For release validation, pass the required `fltk-*` test paths to
`SHELL_TEST_CASES` and `GUI_TEST_CASES` and repeat with `SMP=2`. Keep D1 out
of the default suite unless the port is installed. Use bounded serial
readiness checks and bounded `GUI-CHECK-SCREEN`/`GUI-CHECK-SCREEN-RECT` waits
in `GUI-VISUAL-TEST` fixtures. Use absolute pointer coordinates and stable
pixel regions; pair screenshots with assertions against filesystem data,
telemetry, or process state rather than treating a matching screenshot as
proof that an operation completed.

## Upstream References

- [FLTK 1.4 programming manual](https://www.fltk.org/doc-1.4/intro.html)
- [FLTK license and exceptions](https://www.fltk.org/doc-1.4/license.html)
- [Dillo 3.3.0 release and experimental FLTK support](https://dillo-browser.org/release/3.3.0/)
