# GUI Implementation Roadmap

Part of the [GUI platform plan](README.md). Follow these milestones in order;
each has a concrete deliverable and a validation gate.

This is the sole authority for implementation order, work-item completion,
test gates, and commit boundaries. The companion documents own their specific
contracts: [Kernel foundations](kernel-foundations.md) owns kernel ABI and
state rules, [UI toolkit](ui-toolkit.md) owns `libmario-ui` behavior, and
[Desktop and applications](desktop.md) owns desktop/application behavior.
Correct a contract before scheduling work that depends on it.

This section is the executable form of the plan. Work through the milestones
in order. A milestone may use several small, reviewable commits, but it is not
complete until its full checkpoint is green. Do not start a later milestone
while an earlier checkpoint is red.

## Guide Conventions

- The roadmap is ordered by the **Commit-Sized Execution Plan** below. A work
  item is one implementation commit (or one test-only commit when explicitly
  stated), not a broad milestone-sized change. Do not combine adjacent items
  merely because they touch the same subsystem.
- Before starting any unfinished implementation item, run `make test SMP=16`
  on the preceding commit. Run its focused test while developing, then run
  `make test SMP=16` again before committing. This protects normal shell, TTY,
  and GUI coverage from a temporary GUI-specific kernel configuration.
- Each item must leave the common image usable by the next item. A test that
  needs a changed kernel configuration uses `REBUILD-KERNEL` and restores the
  normal configuration before the next test; it must not rely on a runtime
  debug switch or a manually rebuilt image.
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
- Mario builds one common image. Its GRUB4DOS menu selects VBE with `setvbe`, includes the
  framebuffer/input drivers and GUI payload, and uses F1-F6 versus F8 only as
  a runtime foreground-console transition.
- Text and GUI tests run against that same image. `tests/lit_tty_test.py`,
  `tests/lit_shell_test.py`, and `tests/lit_gui_test.py` run their respective
  selected cases. M0 GUI tests exercise `/dev/fb0` and F1-F6 framebuffer text
  VTs; F8 begins in M1.
- GUI test file names always use lowercase hyphenated stems. Existing names
  include `fb-console-state.test`, `gui-console.test`, and
  `gui-seat-lifetime.test`; reserve future names in the same form, such as
  `gui-desktop.test` and `gui-terminal.test`.
- When a milestone introduces a new GUI binary, update `app/usr/Makefile` and
  `rd/copy_files.py` in that milestone; verify with `rd/make-rd0.py` that
  `make disk-image` installs and strips the binary into the common `ramdisk0`.

## Milestone Map

| Milestone | Deliverable | Exit gate |
| --- | --- | --- |
| M0 — Framebuffer platform | VBE handoff, `/dev/fb0`, and framebuffer text VTs | `fb.test` and `fb-tty.test` |
| M1 — Graphics session and test control | F8 graphics mode, raw input, and GUI-runner primitives | console/input shell and GUI tests |
| M2 — Toolkit and fonts | `libmario-ui`, pinned FreeType, bundled fonts | font and event screenshots |
| M3 — Window manager and shell | `mario-wm`, protocol, shell client, and desktop interaction | desktop and window screenshots |
| M4 — PTY prerequisite gate | existing Unix98 PTY/tmux contract | PTY and tmux regressions |
| M5 — GUI terminal | VT emulator, truecolor, PTY shell, tmux panes | terminal screenshot regression |
| M6 — Image viewer | BMP/PNG decode, fit, zoom, pan | image-viewer screenshot regression |
| M7 — File manager | browse, mutate `/tmp`, and launch apps | file-manager end-to-end regression |
| M8 — Window-system hardening | protocol quotas, disconnect recovery, and server restart | window-system recovery regression |

## Commit-Sized Execution Plan

This is the authoritative work order. The milestone sections below specify the
ABI and behavior in detail; each work item identifies the smallest coherent
change that may be committed. Every unfinished item has this validation shape:

```sh
# Run the named focused test(s) while changing the item.
make test SMP=16
git diff --check
```

The `make test SMP=16` gate is required both before starting the next item and
before committing the current one. Screenshot-producing GUI tests also retain
their actual PPM and serial/QMP diagnostics on failure.

### Work-Item Contract

Every unchecked item below is incomplete until its item text, its milestone
entry in the dependency map, and its commit description/test together answer
all of these questions:

1. **Prerequisite:** name the immediately preceding item(s) whose ABI it uses.
   A step must not add compatibility code for an interface scheduled later.
2. **Owned surface:** list the source root, UAPI/protocol header, and test root
   it may change. Changes outside those roots require splitting the item or
   adding an explicit dependency to the plan before editing.
3. **Success and failure result:** name the visible success result and the
   rollback/error result. In particular, ownership loss must preserve the last
   known-good text VT or committed GUI frame; it may not silently retry an
   unsafe operation.
4. **Focused gate:** identify one test executable/fixture and one test command.
   Pure helpers use host or Mario unit tests; kernel ABI behavior uses a shell
   or GUI guest test; visible behavior uses a QMP screendump assertion.
5. **Commit boundary:** include implementation, comments, focused regression,
   and required install/copy rules together. Do not put a refactor, a new ABI,
   and its first end-to-end behavior in one commit unless the ABI would be
   unusable without all three.

The common full-suite gate follows every focused gate. Within a numbered group,
an item depends on every preceding sibling unless its text explicitly says
otherwise; no item may start while one of those predecessors is red. A work
item that cannot state this contract is planning work, not implementation work.

### Planning Boundaries

- A checked item records code already present on `main`; an unchecked item is
  a forward contract, not permission to depend on an unimplemented interface.
- The detailed specification beneath a milestone explains the final state. Its
  file list and checkpoint do not merge the commit-sized items above it.
- A planned test name is reserved documentation until its file exists. Do not
  change an existing test merely to match a future test name.
- Shared lifecycle vocabulary—graphics seat, completed frame, foreground
  transition, and architectural phases—is defined in [the overview](README.md).
  Work items must preserve those meanings rather than create local variants.

### Ownership and Dependency Map

| Work range | Must be complete first | Primary implementation roots | Focused gate type |
| --- | --- | --- | --- |
| M1.9.2–M1.11 | M1.9.1 and `pread64()`/`pwrite64()` | `src/fs/char/fb.c`, `tests/lit_gui_test.py`, `app/test/`, `tests/gui/` | GUI seat/scanout regression |
| M2 | M1.9.3, M1.10.3, and M1.12.1 | `app/gui/libmario-ui/`, `app/usr/freetype/`, `app/usr/Makefile` | unit test plus one deterministic golden |
| M3 | M2.2.3, M2.3.2, M2.4.2, M2.5.2 | `app/gui/libmario-ui/`, `app/gui/desktop/`, `app/system/init/`, `tests/lit_gui_test.py` | protocol unit tests plus desktop golden |
| M4 | M3.6.2 only | `app/test/pty/`, `tests/shell/`, `tests/tty/` | PTY/tmux regression |
| M5 | M3.6.2 and all M4 gates | `app/usr/terminal/`, `app/gui/libmario-ui/`, `tests/gui/` | parser unit tests plus terminal golden |
| M6 | M3.6.2 and M2.2.2 | `app/usr/imageviewer/`, `app/usr/libpng/`, `tests/gui/assets/` | decoder unit test plus image golden |
| M7 | M3.6.2, M5.1.2, and M6.1.3 | `app/usr/filemanager/`, `tests/gui/assets/files/`, `tests/gui/` | `/tmp` end-to-end fixture |
| M8 | M3.6.2 and M5–M7 complete | `app/gui/libmario-ui/`, `app/gui/desktop/`, `app/usr/` | server-recovery and quota regressions |

M4 is allowed to begin after M3 has a reliable graphics-seat lifecycle so PTY
failures can be diagnosed from a stable desktop. M5, M6, and M7 may proceed in
parallel only after their listed prerequisites are green; their writes must be
disjoint. M8 begins only after the protocol-client applications are stable,
because it hardens their established rendering transport rather than replacing
their behavior.

Before M0:

```sh
make shell-test
make tty-test
```

Record the baseline result. Any failure here is pre-existing and must be fixed
or consciously excluded before GUI work begins.

## M0: Framebuffer Platform

- [x] **M0.1 — framebuffer handoff and mapping.** Validate the Multiboot,
  legacy VBE, and tokenized GRUB4DOS handoffs in `src/video/fb.c`; reject an
  unsafe physical mapping before exposing a device. Test with `fb.test` and
  the no-VBE text-console boot path. Commit: `video: map VBE framebuffer`.
- [x] **M0.2 — shared framebuffer ABI and character device.** Add the
  fixed-width `mario/fb.h` contract, `/dev/fb0`, bounds-checked read/write,
  seek, and format/state ioctls. Test ABI layouts and read/write bounds in
  `app/test/fb` and `tests/gui/fb.test`. Commit: `fb: add shared framebuffer
  ABI`.
- [x] **M0.3 — framebuffer text presenter.** Render text VTs through the
  framebuffer without changing the text-TTY ownership model. Test F1–F6,
  cursor handling, and the preserved VGA fallback in `tests/gui/fb-tty.test`.
  Commit: `fbcon: render text VTs through framebuffer`.

### Detailed Specification

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
  `fb.test` exists. Keep `vga-test` as an intentional no-op compatibility
  target and never attach GUI coverage to it.
- `qemu/grub4dos-0.4.6a/menu.lst`: make GRUB4DOS the default disk loader and
  use `setvbe 1024x768x32`. QEMU's direct `-kernel` Multiboot loader does not
  supply VBE, so GUI runs boot the normal disk image.
- `src/include/multiboot.h`: add framebuffer-extension fields plus the packed
  legacy VBE mode-info structure. Keep the supported `MB_FLAGS=0x00010003`:
  GRUB4DOS rejects the plan's former `0x00010403` header bit.
- `src/include/video/fb.h`: add `struct fb_info`.
- `src/video/fb.c`: add `fb_early_init()` and `fb_map()`. Accept a direct
  Multiboot framebuffer record, a legacy VBE mode-info record, or GRUB4DOS's
  strictly tokenized `video=<mode>@<base>,<pitch>` command-line handoff. Use
  checked parsing and reject malformed/ambiguous values before mapping.
- Create `src/include/uapi/mario/fb.h` and copy it into the userspace include
  tree as `mario/fb.h`. It defines Mario's fixed-width ioctl layouts and
  compile-time layout checks; remove local userspace mirrors and do not
  describe the ABI as Linux-layout compatible.
- `src/kernel/mario.c` or the current kernel entry file: call `fb_early_init()`
  before paging teardown and `fb_map()` after paging is stable. Start the one
  `fbcon` kernel thread only after `sched_init_smp()` and after the init task
  has been created, preserving PID 1 for init. The worker waits on `fbcon_wait`
  and is the sole framebuffer text-painting context. `fbcon_start()` queues a
  full initial `tty1` repaint after the worker starts.
- `src/include/video/fbcon.h` and `src/video/fbcon.c`: add the kernel-only
  framebuffer text presenter. It consumes existing `struct console` cell
  buffers, uses a fixed CP437 bitmap/palette, honors real stride and masks, and
  implements a software cursor without depending on `/dev/fb0` or userspace.
- `src/video/font_cp437_18x32_gray.inc`: add the checked-in 256-glyph bitmap generated
  once from the bundled DejaVu Sans Mono source, with source/license metadata.
- `src/fs/char/console.c`: render the existing 80x25 text-VT cell buffers with
  a VGA-compatible 9x16 CP437 renderer centered in a 9x18 cell, leaving a
  two-unit line gap, whenever a framebuffer is enabled; retain the legacy-VGA
  rendering path when no framebuffer handoff is present. Console writes wake
  the `fbcon` worker after releasing `console_lock`; the worker snapshots the
  foreground 80x25 buffer, compares it with its prior snapshot, and repaints
  only changed cells plus cursor changes. A foreground-VT switch forces a full
  scanout repaint.
- `src/include/fs/chrdev.h`: add `FB_MAJOR 13` and `FB_MINOR 0`.
- `src/fs/char/fb.c`: implement open/lseek/read/write/release/ioctl.
- `rd/device_list`: add `fb0 C 13 0`.
- `tests/gui/fb.test`: switch to an `oksh` text terminal, type
  `dmesg | grep 'fb:'` through QMP key events, then write a known pixel pattern
  through `/dev/fb0` and compare the complete screendump.
- `tests/gui/fb-tty.test`: switch to a text terminal, draw fixed colored
  rectangles in its four corners, and compare complete screendumps with the
  cursor shown and hidden. It starts no GUI-seat helper and proves the
  framebuffer text path independently of the desktop.
- `tests/lit_gui_test.py`: add a dedicated QMP/serial test runner. It uses QMP
  for `screendump` and input injection, serial only for guest control/status,
  and never shares implementation with the tmux text runner.

Build and run:

```sh
make kernel
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test fb.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test fb-tty.test tests/gui
make gui-test
```

Checkpoint:

- Kernel `printk` reports a nonzero width, height, stride, bpp, and physical
  base.
- `fb.test` passes.
- `fb-tty.test` proves a text terminal's 80x25 cell buffer and cursor
  visibility render through the framebuffer path while no `/dev/gui` owner or
  desktop process exists. F8 remains unavailable in M0.
- The same image passes all shell and TTY tests; without VBE information it
  falls back to legacy text output.
- An invalid GRUB4DOS handoff produces the documented serial-recovery failure
  rather than claiming a legacy-VGA fallback that protected mode cannot
  provide. M0 has no injected `fbcon` worker-spawn-failure path; M1.12 adds
  the checked fail-closed halt regression and must not claim a synchronous
  presenter fallback.
- `fb.test` performs its raw write/readback before M1 installs foreground-GUI
  write gating; M0 proves the device ABI, not desktop ownership.

Debug:

- If the screen stays black, verify `MB_FLAGS` and the mode word order.
- If the framebuffer is mapped at the wrong address, compare the VBE base
  against the reserved `0xf0400000` kernel window. It must not share AHCI's
  single-slot `ioremap()` aperture at `0xf0000000`.
- If `/dev/fb0` reads garbage, check stride versus width and use
  `bytes_per_scanline` for every row.

Suggested commit:

```text
video: map VBE linear framebuffer and add /dev/fb0
```

## M1: Graphics Session, Input, and Test Control

- [x] **M1.1 — graphics-seat state.** Introduce explicit text/graphics
  foreground state, preserve the prior text VT, and make F8 a graphics-seat
  request rather than a tty index. Test F1–F8 routing in
  `tests/gui/gui-console.test`. Commit: `console: add graphics seat
  foreground mode`.
- [x] **M1.2 — input UAPI.** Publish stable keyboard and mouse event layouts
  in `mario/input.h`, with compile-time size checks shared by kernel and
  userspace. Test with `app/test/input_abi`. Commit: `gui: define input event
  UAPI`.
- [x] **M1.3 — keyboard endpoint.** Implement exclusive `/dev/gui` opens,
  complete-record reads, `poll()`, overflow reporting, and F1–F12 reservation.
  Test keyboard routing, invalid read sizes, and no text-TTY leakage in
  `tests/gui/gui-input.test`. Commit: `gui: deliver keyboard events through
  graphics seat`.
- [x] **M1.4 — completed-transition observation.** Add
  `FBIOGET_CONSOLE_STATE` and `/dev/fb0` `POLLPRI` only after a completed
  repaint. Test state transitions in `tests/gui/fb-console-state.test`.
  Commit: `fb: report completed console transitions`.
- [x] **M1.5 — framebuffer write ownership.** Reject user framebuffer writes
  outside a completed graphics foreground state and keep kernel fbcon writes
  internal. Test rejection and F1/F8 transition behavior in
  `tests/gui/fb.test`. Commit: `fb: gate writes across console transitions`.
- [x] **M1.6 — seat lifetime.** Tie graphics ownership to the final shared
  file-description reference, including `dup()` and `fork()`. Test final-close
  restoration and F8 no-op behavior in `tests/gui/gui-seat-lifetime.test`.
  Commit: `gui: test seat ownership lifetime`.
- [x] **M1.7 — PS/2 mouse endpoint and injection.** Add `/dev/psaux`, safe
  auxiliary-controller probing, IRQ12 delivery, fixed-record queue semantics,
  and runner mouse directives. Test real injected packets in
  `tests/gui/psaux.test`. Commit: `input: add psaux mouse events`.
- [x] **M1.8 — positional-I/O core.** Add `read_at`/`write_at` operations to
  `struct file_operations`; implement `pread64()` and `pwrite64()` in
  `src/fs/read_write.c` and the i386 syscall table without changing `f_pos`.
  Convert regular-file implementations before any device uses the callbacks.
  The focused userspace regression covers success, EOF short reads, invalid
  i386 offsets, `O_APPEND` separation, and unchanged offsets across MarioFS,
  ext2, and vfat. The callback contract propagates a short count or `EINTR`
  without retrying; deterministic interrupted-flush retry coverage belongs to
  M2.3, where a controllable sink exists. Commit: `fs: add positional I/O
  callbacks`.
- [x] **M1.9.1 — extract framebuffer transfer helpers.** Refactor
  `src/fs/char/fb.c` so ordinary `read()` and `write()` pass an explicit local
  position to shared bounds-checking/copy helpers. Preserve current behavior:
  only a successful ordinary operation advances `f_pos`, a text foreground
  still rejects writes with `EBUSY`, and EOF/`ENOSPC` results are unchanged.
  Run `tests/gui/fb.test` and `make test SMP=16`. Commit: `fb: share transfer
  position handling`.
- [x] **M1.9.2 — expose positional framebuffer callbacks.** Register
  `read_at`/`write_at` on `/dev/fb0`; the callbacks must use the exact same
  foreground-state, transition, range, and short-count rules as M1.9.1 while
  updating only the syscall-local position. **Owned surface:**
  `src/fs/char/fb.c`, `app/test/fb-positional/`, and one serial GUI fixture.
  The probe must prove text-mode `EBUSY`, completed-transition success, and
  readback before adding a visual comparison. Run that fixture and
  `make test SMP=16`. Commit: `fb: add gated positional callbacks`.
- [x] **M1.9.3 — prove shared-descriptor semantics visually.** Add a dedicated
  GUI test process that owns `/dev/fb0` and `/dev/gui`, waits for the F8
  `POLLPRI` transition, sets `f_pos` to one scanline, writes a different
  scanline with `pwrite64()`, then checks it with `pread64()`. The process must
  prove `lseek(SEEK_CUR)` is unchanged by positional success and failure, and
  the GUI test must compare the two distinct scanline colors through QMP.
  **Owned surface:** the M1.9.2 probe plus `tests/gui/fb-positional.test`.
  Run `tests/gui/fb-positional.test` and `make test SMP=16`. Commit:
  `test: cover framebuffer positional I/O`.
- [x] **M1.10.1 — snapshot a write generation.** Record a completed graphics
  generation before a user framebuffer copy and reject the operation if it is
  no longer current before its first byte. Unit test the state helper without a
  long copy. Commit: `fb: snapshot write generation`.
- [x] **M1.10.2 — bound framebuffer copy chunks.** Copy user data in at most
  one scanline per lock hold, check the generation between chunks, and return
  the completed byte count rather than copying stale bytes. Test short final
  rows and an injected generation change. Commit: `fb: bound framebuffer write
  chunks`.
- [x] **M1.10.3 — add the transition-race regression.** Drive a large write,
  inject F1 then F8 at deterministic chunk boundaries, and prove text repaint
  wins on switch-away while the following completed GUI frame has no stale text
  rows. Commit: `test: cover framebuffer transition races`.
- [x] **M1.11.1 — parse visual assertions centrally.** Retain the existing
  full-screen `GUI-CHECK-SCREEN` and solid-color `GUI-CHECK-RECT` assertions;
  add parser validation for `GUI-VISUAL-TEST:`, `GUI-CHECK-SCREEN-RECT`,
  `GUI-CHECK-DIFFERENT`, and `GUI-CHECK-MATCH`, including missing assets and
  invalid geometry. Unit test directive parsing without QEMU. Commit: `test:
  validate GUI visual directives`.
- [x] **M1.11.2 — require visuals only for declared desktop/app tests.** A
  no-argument `GUI-VISUAL-TEST:` marker declares a visible integration test and
  requires at least one screenshot comparison. It appears exactly once; if
  present, `REBUILD-KERNEL` remains the first directive and the marker must
  precede every guest or QMP action. Keep raw QMP device/ABI fixtures—such as
  `fb.test`, `gui-input.test`, and `psaux.test`—in `tests/gui/` without a
  marker or screenshot requirement. Test accepted marked/unmarked fixtures and
  rejected duplicate, argument-bearing, late, and screenshot-free markers.
  Commit: `test: require declared GUI visuals`.
- [x] **M1.11.3 — retain generic GUI failure diagnostics.** Keep the actual
  PPM, serial tail, and QMP state for visual failures. Test runner cleanup and
  diagnostics with a failing fixture without requiring a desktop process.
  Commit: `test: retain GUI failure diagnostics`.
- [x] **M1.11.4 — add serial readiness waits.** Implement
  `SERIAL-WAIT-FILE` through the guest serial shell; the host must never read a
  guest path directly. Test ready, missing, and stale-file fixtures. Commit:
  `test: wait for GUI ready`.
- [x] **M1.11.5 — add completed-frame waits.** Implement `GUI-WAIT-FRAME` by
  snapshotting a decimal frame counter and requiring a greater atomic value.
  Test advance, malformed content, removal, and timeout. Commit: `test: wait
  for GUI frames`.
- [x] **M1.12.1 — halt if the framebuffer presenter cannot start.** Change the
  kernel-thread creation boundary used by `fbcon_start()` to return a checked
  failure result. If the sole framebuffer text presenter cannot be created
  after VBE has been selected, print one concise serial diagnostic and halt;
  do not fall back to legacy VGA or synchronous rendering. Add an injected
  spawn-failure fixture using `REBUILD-KERNEL` and `EXPECT-HALT`. Commit:
  `fbcon: halt when presenter startup fails`.

### Detailed Specification

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
- `src/include/uapi/mario/input.h`: define the fixed-size 8-byte key and
  12-byte mouse records, flags, and compile-time layout assertions; install it
  as `mario/input.h` for applications. `src/include/fs/gui.h` and
  `src/include/fs/psaux.h` retain only kernel queue/driver state.
- `src/include/fs/gui.h`: define the `GUI_KEY_EVENT_OVERFLOW` flag,
  256-record queue, `gui_init()`, keyboard-queue, and foreground-state
  interfaces used by the console and keyboard drivers.
- `src/fs/char/gui.c`: register `/dev/gui` at `GUI_MAJOR 11`, `GUI_MINOR 0`;
  implement its exclusive read-only open, record-multiple `read()` semantics,
  and `poll()`.
- `src/fs/char/tty.c`, `src/fs/char/console.c`, and `src/fs/char/kbd.c`:
  initialize the GUI device before PS/2 input is enabled, reserve F1-F12
  before looking up a tty, route F1-F6 through `switch_text_console()` and F8
  through `enter_graphics_console()`, leave F7/F9-F12 as reserved no-ops, and
  enqueue non-function keys only while graphics mode is foreground. In graphics
  mode, the last text tty must receive no keyboard bytes. Notify the desktop on
  switch-away/switch-back.
- `src/include/video/fb.h` and `src/fs/char/fb.c`: add
  `FBIOGET_CONSOLE_STATE` and wake `/dev/fb0` pollers with `POLLPRI` on
  console switches. M0 permits the focused raw `fb.test` writer; after M1,
  ordinary `/dev/fb0` writes return `-EBUSY` unless graphics is foreground.
  The kernel `fbcon` presenter bypasses this file-operation gate. M1.10
  converts user writes to bounded scanline-sized chunks with a
  foreground-generation check, so transition repaint wins over an in-flight
  stale GUI write only after that sub-milestone is complete.
- `src/fs/read_write.c` and the i386 syscall table: implement `pread64()` and
  `pwrite64()` through explicit `read_at`/`write_at` callbacks with a local
  `off64_t`. They preserve `f_pos`, handle short counts and `EINTR`, reject
  invalid i386 high/low offsets, and are not seek/write/restore wrappers.
  Make the framebuffer positional callback use the normal foreground-generation
  gate. Add a regression that shares one framebuffer fd between offset-based
  and positional operations and proves that `pwrite64()` neither changes the
  observed offset nor corrupts the selected scanline.
- `src/include/fs/psaux.h` and `src/fs/char/psaux.c`: enable the auxiliary
  PS/2 port, decode the basic 3-byte packet into the fixed 256-record
  `mario_psaux_event` queue, set `PSAUX_EVENT_OVERFLOW` after a drop, and
  implement its exclusive read/poll interface. `dz` is zero; wheel negotiation
  is out of scope for this milestone.
- `src/include/fs/chrdev.h`: add `GUI_MAJOR 11`, `GUI_MINOR 0`,
  `PSAUX_MAJOR 10`, and `PSAUX_MINOR 1`.
- `rd/device_list`: add `gui C 11 0` and `psaux C 10 1`.
- `app/test/gui-seat/main.c`: add a deliberately small ABI probe that opens
  `/dev/fb0` for exclusive write access, claims `/dev/gui`, reports state over
  the serial shell, and blocks until signalled. It is used only until M3 proves
  the real desktop owner; it does not draw or bypass routing.
- `tests/gui/gui-console.test`: verify F1-F6 and F8 switching, and verify
  F7/F9-F12 neither change the foreground console nor reach `/dev/gui`.
- `tests/gui/gui-seat-lifetime.test`: verify a `dup()` and a forked child retain the
  GUI seat after the parent's close, then verify the final close restores the
  prior text VT and makes F8 a no-op until a new owner claims both devices.
- Extend `gui-console.test` with an intentionally large framebuffer write that
  overlaps an injected F1/F8 transition. The foreground-generation gate must
  ensure no stale desktop scanlines survive the text repaint and no stale text
  scanlines survive the next completed GUI frame.
- `tests/shell/mouse.test`: use the QEMU PS/2 mouse fixture to verify `poll()`,
  full `mario_psaux_event` reads, invalid read-count rejection, and
  overflow-state recovery.
- `tests/lit_gui_test.py`: add the GUI readiness, frame-wait, mouse-input, and
  pixel-rectangle directives specified in the Validation section. The runner
  owns QMP input injection and screendumps; test files name only GUI actions.

Build and run:

```sh
make kernel
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-console.test tests/gui
.venv/bin/python tests/lit_shell_test.py tests/shell/mouse.test
```

Checkpoint:

- F1-F6 still display the existing text consoles.
- F8 selects the graphics console only while `/dev/gui` has an owner; F1-F6
  restore the selected text VT and its framebuffer-rendered contents.
- `gui-console.test` verifies F8 first produces the deterministic black
  graphics scanout before any desktop exists, rather than retaining pixels from
  the prior text VT. It also verifies F1-F6 repaint the selected text VT before
  the `/dev/fb0` `POLLPRI` notification is observed.
- `gui-console.test` starts `gui-seat` through serial before injecting F8, then
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
- `pread64()`/`pwrite64()` are available before M3. The framebuffer positional
  I/O regression verifies short writes, interruption handling, and unchanged
  `f_pos` on success and failure.
- Kernel and userspace compile the same `mario/input.h` layouts. `/dev/gui`
  and `/dev/psaux` reject partial-record reads, return complete FIFO records in
  order, and preserve the documented overflow marker.

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

- [x] **M2.1.1 — pin the font source.** Add the FreeType source archive,
  version, URL, checksum, license note, and an offline checksum verifier.
  Test only source acquisition/verification. Commit: `gui: pin freetype
  source`.
- [x] **M2.1.2 — cross-build a minimal FreeType library.** Build the required
  SFNT/TrueType/CFF modules as a static library through `mario-musl-cc`; do not
  link it into an application yet. Test archive symbols and deterministic
  rebuild output. Commit: `gui: build freetype for Mario`.
- [x] **M2.1.3 — install deterministic font assets.** Add the bundled TTF and
  copy/install rules, then use a smoke executable to open its face and report
  fixed metrics. Test the installed ramdisk path. Commit: `gui: install
  bundled font`.
- [x] **M2.2.1 — define `gui_surface` ownership.** Add XRGB8888 allocation,
  stride, explicit destruction, and overflow-safe dimension validation. Unit
  test zero, maximum, and allocation-failure dimensions. Commit: `gui: add
  owned software surfaces`.
- [x] **M2.2.2 — add clipped drawing primitives.** Implement rectangle
  intersection, fill, and opaque blit without damage tracking. Unit test empty
  clips, all four edges, and overlapping source/destination regions. Commit:
  `gui: add clipped surface primitives`.
- [x] **M2.2.3 — add damage regions.** Introduce bounded/coalesced dirty
  rectangles with a documented full-surface fallback when the list is full.
  Unit test merge order, containment, and fallback. Commit: `gui: track
  surface damage`.
- [x] **M2.3.1 — flush one damage rectangle.** Copy one clipped region with
  `pwrite64()` and retain it when the framebuffer returns `EBUSY`. Test short
  writes and an interrupted copy through a mock sink. Commit: `gui: flush one
  damaged rectangle`.
- [x] **M2.3.2 — flush a damage set.** Retry remaining regions in order, clear
  only completed damage, and preserve the full set across a foreground change.
  Test partial completion and transition retry. Commit: `gui: flush damaged
  surface regions`.
- [x] **M2.4.1 — decode and measure text.** Define UTF-8 replacement behavior,
  CP437 fallback selection, glyph metrics, and baseline layout independent of
  rasterization. Unit test malformed UTF-8 and fallback choice. Commit: `gui:
  add text layout`.
- [x] **M2.4.2 — rasterize and cache glyphs.** Add a bounded LRU glyph cache
  over FreeType with deterministic eviction and theme colors. Test cache hit,
  eviction, and one pixel-exact glyph golden. Commit: `gui: render cached
  glyphs`.
- [x] **M2.5.1 — normalize input records.** Convert `/dev/gui` and
  `/dev/psaux` records into toolkit events, including overflow reset, without a
  desktop policy. Test FIFO drain and reset ordering. Commit: `gui: normalize
  seat input events`.
- [x] **M2.5.2 — add the event-loop dispatcher.** Wrap `poll()` registrations,
  timer deadlines, and callback removal; define callback ordering on one wake.
  Unit test wakeup, timer expiry, and self-removal. Commit: `gui: add event
  loop dispatcher`.

### Detailed Specification

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
- `app/gui/libmario-ui/include/gui/*.h`: define the core types and public API from
  the design section.
- `app/gui/libmario-ui/src/surface.c`: fill, blit, blend, lines, clipping.
- `app/gui/libmario-ui/src/font.c`: FreeType loading, UTF-8 conversion, glyph cache,
  CP437 fallback, fixed pixel-size selection, and deterministic grayscale
  no-hinting raster settings. Cache 8-bit alpha glyph bitmaps keyed by face ID
  rather than misusing a 32-bit `gui_surface` for glyph coverage.
- `app/gui/libmario-ui/src/event.c`: keyboard/mouse translation and event queue.
- `app/gui/libmario-ui/src/widget.c`: first `gui_label`/`gui_button` widgets.
- `app/gui/libmario-ui/assets/fonts/`: bundle the pinned DejaVu Sans and
  DejaVu Sans Mono sources; `rd/copy_files.py` installs them under
  `/usr/share/fonts` in the common image.
- `tests/gui/gui-font.test`: render a known string and compare the screen.
- `tests/gui/gui-font.test`: also render the same codepoint in DejaVu Sans and
  DejaVu Sans Mono to prove that cache entries are face-specific.
- `tests/gui/gui-event.test`: inject keyboard/mouse input and verify a moving
  cursor or button state, focus-reset behavior, and cancellation of a drag
  during a graphics/text transition.

Build and run:

```sh
make app
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-font.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-event.test tests/gui
```

Checkpoint:

- A standalone GUI smoke app renders TTF text without crashing.
- The CP437 fallback works when a codepoint is missing from the TTF.
- `gui_surface` conversions produce the same color on 32, 24, and 16 bpp
  fallback modes.
- Keyboard and mouse events are timestamped and delivered in poll order.
- Font goldens use the pinned font files, integer pixel size, and fixed
  grayscale raster mode; changing any of these requires intentional golden
  regeneration and review.

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

## M3: Window Manager and Shell

- [x] **M3.1.1 — define the wire ABI and endpoint roles.** Add protocol
  version, message enum, packed payload layouts, checked length arithmetic,
  and encode/decode unit tests. Define server-assigned ordinary versus shell
  endpoints; no client-controlled `HELLO` field may claim the shell role.
  Commit: `gui: define window-manager protocol`.
- [x] **M3.1.2 — reassemble framed input.** Implement fragmented receive,
  message-size limits, and bounded per-client input buffering. Test split,
  oversized, malformed, and EOF frames. Commit: `gui: reassemble protocol input`.
- [x] **M3.1.3 — queue framed output.** Add one bounded nonblocking send queue
  per endpoint with `POLLOUT` continuation and motion coalescing. Test short
  sends, queue exhaustion, and ordering. Commit: `gui: queue protocol output`.
- [x] **M3.2.1 — start `mario-wm` as sole seat owner.** Install the server,
  reserve `/dev/fb0` and `/dev/gui` before F8, bind the pathname socket, and
  remove stale server status files. Test text boot, F8 eligibility, and failed
  startup rollback. Commit: `gui: start window manager server`.
- [x] **M3.2.2 — make server cleanup failure-safe.** Close seat/listener fds,
  remove the socket/status files, restore the prior text VT, and prove restart
  after startup or loop failure. Commit: `gui: clean up window manager seat`.
- [x] **M3.2.3 — negotiate ordinary windows.** Implement `HELLO`, client
  lifecycle, create/destroy windows, and server-owned configure serials. Test
  one synthetic public client create/focus/close path. Commit: `gui: negotiate client windows`.
- [x] **M3.2.4 — reset input at ownership boundaries.** Deliver ordered
  `INPUT_RESET` on focus, grab, disconnect, and seat changes before later
  events. Unit test exact ordering. Commit: `gui: reset client input state`.
- [x] **M3.3.1 — stage client surfaces.** Validate dimensions against scanout
  budgets and accept only a configured `SURFACE_BEGIN`. Test malicious
  dimensions and allocation exhaustion. Commit: `gui: stage client surfaces`.
- [x] **M3.3.2 — transfer and commit client frames.** Require ordered,
  nonduplicate `SURFACE_DATA` rows and a matching `SURFACE_COMMIT`; retain the
  old frame on failure/disconnect. Test gaps, stale serials, and one visible
  update golden. Commit: `gui: commit client surfaces atomically`.
- [x] **M3.4.1 — start the private shell client.** After the public listener is
  ready, create a `socketpair()`, mark the server endpoint as the sole shell
  capability, and `exec` `mario-desktop --wm-fd=3` with only its connected fd
  3. Test that public clients cannot claim the role and no client receives raw
  device/listener fds; an initial shell that cannot become shell-ready (`HELLO`
  plus a full desktop commit) returns the session to text without a retry loop.
  Commit: `desktop: start shell through window manager`.
- [x] **M3.4.2 — commit the desktop surface and first frame.** Accept one
  shell-only `DESKTOP_CREATE`, composite its background beneath ordinary client
  surfaces, and publish ready/frame status only after a successful full flush.
  Add one desktop golden; reject a public `DESKTOP_CREATE`; prove `EBUSY` does
  not advance the frame. Commit: `gui: compose shell desktop surface`.
- [x] **M3.4.3 — draw shell panel and server cursor.** Have the shell draw the
  panel, fixed `--:--` clock, and launcher into its desktop surface while the
  server draws the cursor. Add panel/cursor goldens. Commit: `desktop: draw shell panel`.
- [x] **M3.5.1 — add server window storage and paint order.** Define window
  lifetime, rectangles, clipping, and top-to-bottom z-order. Test overlap and
  occlusion pixels. Commit: `gui: compose window stack`.
- [x] **M3.5.2 — add focus, shell taskbar, and client input delivery.**
  Implement click/focus, MRU ordering, focused-client key routing, and ordered
  shell-only window-list notifications/activate requests. The server intercepts
  Alt-Tab and Super-W before client delivery. Test focus change, input
  isolation, taskbar activation, and rejected public shell controls. Commit:
  `gui: route focused client input`.
- [x] **M3.5.3 — add move, cooperative close, and resize.** Implement hit
  testing, capture, server-owned configure delivery, and old/new rectangle
  damage. A title-bar close or Super-W sends `WINDOW_CLOSE_REQUEST`; only the
  owner may destroy its window and M3 has no forced-close timeout. Test
  press/drag/release, every resize corner, accepted/deferred close, and minimum
  size. Commit: `gui: interact with client windows`.
- [x] **M3.6.1 — add shell diagnostics and launcher placeholders.** Create
  `Welcome`, `Display`, and error windows as normal shell-client windows. Send
  typed launcher requests to the server; only the server forks, reaps, and
  starts approved client binaries. Test launcher action-to-window mapping.
  Commit: `desktop: add shell diagnostics`.
- [x] **M3.6.2 — preserve session state over F1/F8 and one shell relaunch.**
  Recompose committed client surfaces after re-entry without publishing a stale
  frame. On shell disconnect retain ordinary clients and the last desktop
  snapshot, launch one replacement shell, then atomically replace the snapshot
  after its first commit; end the session if that replacement cannot become
  ready. Test switch-away/re-entry, normal-client disconnect, and shell
  relaunch recovery. Commit: `gui: recover window manager session`.

### Detailed Specification

Entry conditions:

- M0's shared framebuffer UAPI layout tests are green.
- M1's F8/graphics-seat lifetime and framebuffer transition tests are green.
- M1's `pread64()`/`pwrite64()` regression is green; a libc `pwrite()` call
  must not return `ENOSYS` on Mario.
- The GUI runner has `SERIAL-WAIT-FILE` and `GUI-WAIT-FRAME`; screenshot tests
  do not rely on a fixed sleep for desktop synchronization.
- `GUI-VISUAL-TEST:` is available and every M3 desktop/application fixture
  declares it before its actions (after an optional first `REBUILD-KERNEL`).

Goal:

- Create `mario-wm` as the sole graphics-seat owner and `mario-desktop` as its
  first shell client.
- Implement the framed client protocol, server-side composition, and
  move/resize/raise/close/focus without a later transport migration.

Change:

- `app/gui/libmario-ui/protocol.c` and `include/gui/protocol.h`: framed
  protocol encode/decode, bounded queues, version negotiation, server-assigned
  endpoint roles, window/configure/close-request messages, `INPUT_RESET`, shell
  window-list notifications, activate/launch request/result messages, and
  surface transactions.
- `app/gui/desktop/wm_main.c` and `wm_*.c`: server startup/rollback, raw-seat
  ownership, public listener plus private shell `socketpair()`, client
  lifecycle, z-order, focus, hit testing, composition, `/dev/fb0` flush,
  readiness/frame/debug status, and F1/F8 recovery. The server accepts exactly
  one non-decorated bottom `DESKTOP_CREATE` on its private shell endpoint,
  sends that endpoint ordered window-list notifications, and handles typed
  launch requests. On shell disconnect it retains only the last committed
  desktop surface while starting one replacement shell; a replacement that
  cannot become shell-ready ends the session without a retry loop. After an
  F8-triggered completed
  composition, atomically publish `/tmp/mario-wm.frame`, then create
  `/tmp/mario-wm.ready`; remove `ready` on switch-away and all status/socket
  files on clean exit.
- `app/gui/desktop/shell_main.c` and `shell_*.c`: the `mario-desktop` shell
  client, its private inherited `--wm-fd=3` connection, full-screen desktop/
  background/panel surface, static clock, taskbar, launcher requests, and
  `Welcome`/`Display` diagnostic windows. It uses normal framing and surface
  transactions but never opens raw devices or the listener.
- `app/usr/Makefile` and `rd/copy_files.py`: install both `/usr/bin/mario-wm`
  and `/usr/bin/mario-desktop` in the common image.
- `app/system/init/main.c`: start `mario-wm` in a new session after mounting
  `/usr`; the server starts the shell client with a private `socketpair()` only
  after its public listener is ready, and init applies the existing three-second
  server-respawn rate limit.
- `tests/gui/gui-desktop.test`: a `GUI-VISUAL-TEST:` fixture that verifies the
  shell desktop/panel surface, cursor, taskbar activation, and window
  raise/focus behavior.
- `tests/gui/gui-window.test`: a `GUI-VISUAL-TEST:` fixture that verifies
  window move, resize, stacking, and damage-region redraws with exact
  screenshot crops.
- `app/gui/desktop/test/startup.c`: verify each server startup allocation/open
  failure closes previously acquired resources, returns to the text-seat state,
  and leaves no stale socket or `mario-wm` status file. Protocol unit tests
  reject public shell-role, desktop-surface, and taskbar-control attempts.

Build and run:

```sh
make app
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-desktop.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-window.test tests/gui
```

Checkpoint:

- F8 shows the server-composited shell; F1 returns to text and F8 back restores
  the committed client windows.
- The runner records the current frame number, sends F8, waits for
  `/tmp/mario-wm.ready`, then waits for a monotonically newer
  `/tmp/mario-wm.frame` after every injected GUI action. `ready` exists
  only while graphics is foreground and only after its corresponding full
  composition completed. A missing frame file is baseline zero; serial control
  can read all three status files.
- `Welcome` and `Display` begin at their documented locations and exercise
  focus, move, resize, raise, and close without requiring a later application
  milestone. The shell's taskbar tracks each ordinary window from server
  notifications without receiving another client's surface or descriptors.
- If the shell exits, ordinary client windows remain live while the last shell
  desktop surface remains visible but input-inert. A replacement shell receives
  the current window list and replaces that snapshot only after its first
  committed desktop frame; a shell that cannot become shell-ready returns the
  session to text.
- A click raises/focuses a window; Alt-Tab cycles focus; Super-W sends one
  cooperative close request. A window remains visible until its owner destroys
  it or disconnects.
- Moving a window redraws only the dirty area well enough to avoid visible
  stale pixels.
- The cursor remains above all application windows.
- Linked server-startup tests prove an allocation/open failure never leaves a
  claimed graphics seat or stale socket: all acquired resources roll back and
  the prior text VT remains selectable. No production runtime fault trigger
  exists.

Debug:

- Stale pixels usually mean damage regions are not clipped through the window
  stack correctly. Dump damage rects behind a debug flag before rewriting the
  compositor.
- If focus changes but the title bar does not, redraw chrome when focus state
  changes rather than only when application content changes.

Suggested commit:

```text
gui: add window manager server and shell client
```

## M4: PTY Prerequisite Gate

- [x] **M4.1.1 — test PTY allocation and controlling terminals.** Cover
  master/slave allocation, `setsid()`, `TIOCSCTTY`, and terminal attributes.
  Commit: `test: cover terminal PTY setup`.
- [x] **M4.1.2 — test PTY resize and hangup.** Cover `TIOCSWINSZ`, `SIGWINCH`,
  slave close, master EOF, and child reaping. Commit: `test: cover PTY resize
  and hangup`.
- [x] **M4.1.3 — prove tmux's minimum contract.** Turn the existing tmux test
  into explicit prerequisites: pane output, resize, and clean server exit. Do
  not change GUI code. Commit: `test: define tmux PTY prerequisites`.
- [x] **M4.2 — audit proven PTY gaps.** M4.1 passed on SMP=16: `pty.test`
  covers allocation, controlling terminals, terminal attributes, resize,
  `SIGWINCH`, EOF, and child reaping; `tmux-1.test` covers pane output, resize,
  and clean server exit. No PTY implementation gap was found, so this audit
  adds no implementation commit.

### Detailed Specification

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
make tty-test TTY_TEST_CASES=tests/tty/tmux-1.test
```

Checkpoint:

- tmux creates panes, splits a window, creates a second window, returns to the
  split window, and leaves its server alive.
- The checked tmux panel baseline contains the split borders, pane output, and
  status bar.
- Any PTY regression is fixed in the PTY/tmux work before beginning M5.

## M5: GUI Terminal

- [x] **M5.1.1 — create a terminal protocol client.** Connect to `mario-wm`,
  create a window through the protocol, render a deterministic placeholder
  grid, and clean up on close; no PTY yet. Add a `GUI-VISUAL-TEST:` opening and
  closing regression. Enable `LAUNCH_TERMINAL` only after the server can exec
  this client without inherited seat/listener/client fds. Commit:
  `terminal: add window client`.
- [x] **M5.1.2 — attach a PTY child.** Allocate the PTY, establish the
  controlling terminal, spawn the shell, and drain plain output. Test a fixed
  command and child-exit status. Commit: `terminal: attach shell PTY`.
- [x] **M5.2.1 — parse printable text and basic controls.** Implement UTF-8
  feed, CR/LF/BS, wrapping, scrolling, and cursor movement in a pure module.
  Add table-driven parser tests. Commit: `terminal: parse basic text`.
- [x] **M5.2.2 — parse CSI erase and cursor sequences.** Add CUP/CHA/CUU/CUD/
  CUF/CUB plus ED/EL and scroll regions. Test each sequence independently.
  Commit: `terminal: parse CSI cursor and erase`.
- [x] **M5.2.3 — parse attributes and screen buffers.** Add SGR, save/restore
  cursor, alternate screen, and reset. Test state transitions before rendering.
  Commit: `terminal: parse attributes and alternate screen`.
- [x] **M5.3.1 — render terminal cells.** Map parser cells to toolkit glyphs,
  colors, cursor, and CP437 fallback. Add a stable text golden. Commit:
  `terminal: render terminal cells`.
- [x] **M5.3.2 — propagate resize.** Convert window dimensions to rows/columns,
  send `TIOCSWINSZ`, handle `SIGWINCH`, and redraw. Test child-side
  `TIOCGWINSZ` reporting and the resized terminal frame. Commit:
  `terminal: resize PTY grid`.
- [x] **M5.3.3 — add scrollback and tabs.** Keep bounded scrollback and tab
  stops without selection/clipboard. Test scrollback bounds and tab commands.
  Commit: `terminal: add scrollback and tabs`.
- [x] **M5.4.1 — add titles and direct color.** Implement OSC title and RGB
  SGR parsing/rendering. Test one title update and truecolor golden. Commit:
  `terminal: support titles and RGB`.
- [x] **M5.4.2 — validate tmux and full-screen apps.** Add only the DEC modes
  required by `tmux`, `less`, and `vim`; each new mode gets a parser regression.
  Commit: `terminal: support tmux compatibility`.

### Detailed Specification

Goal:

- Implement a terminal as a `mario-wm` protocol client plus VT emulator.
- Fork `/bin/oksh` on a PTY slave and submit the resulting terminal surface to
  its GUI window.

Change:

- `app/usr/terminal/main.c`: connect to `mario-wm`, create/commit its client
  surface, open the PTY, fork the shell, and run the combined socket/PTY loop.
  It never opens `/dev/fb0`, `/dev/gui`, or `/dev/psaux`.
- `app/usr/terminal/vt.c`: cursor, erase, scroll, 16/256/direct-RGB SGR in
  semicolon and colon forms, alternate screen, tabs, and terminal status
  responses.
- `app/usr/terminal/term.c`: terminal grid, scrollback, resize.
- `app/usr/Makefile`: add the terminal binary.
- `app/usr/build-ncurses.sh`: enable ncurses extended and direct RGB colors.
- `rd/copy_files.py`: include the ncurses `xterm-direct` terminfo entry.
- `tests/gui/gui-terminal.test`: a `GUI-VISUAL-TEST:` fixture that opens a
  terminal, runs commands, resizes it, and checks the screen.

Build and run:

```sh
make app
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-terminal.test tests/gui
```

Checkpoint:

- `oksh`, `vim`, `top`, `less`, and interactive Python render acceptably.
- Resizing the window changes the child-visible `TIOCGWINSZ` size and redraws
  the grid.
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

- [x] **M6.1.1 — install immutable image fixtures.** Add tiny/large BMP and PNG
  files with recorded dimensions and hashes; copy them into the common image.
  Test fixture installation only. Commit: `imageviewer: install test assets`.
- [x] **M6.1.2 — parse bounded BMP metadata.** Validate signatures, dimensions,
  row stride, pixel offset, and allocation limits without rendering. Unit test
  each malformed-header rejection. Commit: `imageviewer: validate BMP input`.
- [x] **M6.1.3 — render BMP pixels in a client window.** Connect to
  `mario-wm`, decode supported BMP pixels into a client surface with
  aspect-preserving fit-to-window, and add one `GUI-VISUAL-TEST:` BMP golden.
  Enable the bounded `LAUNCH_IMAGE_VIEWER` path form. Commit:
  `imageviewer: display BMP images`.
- [x] **M6.2.1 — pin and build PNG support.** Add the selected decoder and its
  checksum/build rule independently of the viewer. Test a decoder smoke
  executable. Commit: `imageviewer: build PNG decoder`.
- [x] **M6.2.2 — decode bounded PNG frames.** Enforce dimensions, row bytes,
  and allocation limits; leave the prior frame untouched on a decode error.
  Unit test corrupt/truncated PNGs. Commit: `imageviewer: validate PNG input`.
- [x] **M6.2.3 — render PNG and bound the cache.** Add PNG display plus exactly
  current/next decoded-frame caching and deterministic eviction. Test PNG
  golden and cache replacement. Commit: `imageviewer: display PNG images`.
- [x] **M6.3.1 — add view transforms.** Implement fit, 1:1, bounded zoom, and
  viewport clipping with no navigation. Test transform math and zoom golden.
  Commit: `imageviewer: add zoom modes`.
- [ ] **M6.3.2 — add pan input.** Implement mouse-drag and arrow-key pan with
  clamped bounds. Test each input path and edge clamping. Commit: `imageviewer:
  pan scaled images`.
- [ ] **M6.3.3 — add sibling navigation.** Traverse same-directory image files
  in `readdir()` order, skip unsupported inputs, and keep the old image on
  error. Test next/previous and a corrupt sibling. Commit: `imageviewer:
  navigate sibling images`.

### Detailed Specification

Goal:

- Vendor and build libpng as a static Mario library.
- Decode BMP/PNG and implement fit/zoom/pan/next/previous.

Change:

- `app/usr/libpng/`: vendor pinned sources.
- `app/usr/build-libpng.sh`, `app/usr/libpng.mk`.
- `app/gui/libmario-ui/src/image.c`: BMP/PNG decode to `gui_surface`.
  Enforce BMP file-length/dimension checks and libpng user limits before any
  image allocation; decode PNG rows incrementally into the bounded downscaler.
- `rd/copy_files.py`: install `tests/gui/assets/` under
  `/usr/share/mario-tests/gui`, so guest tests can copy its
  immutable fixture tree to `/tmp/gui-fixtures/` before every mutation.
- `app/usr/imageviewer/`: a `mario-wm` protocol client, viewer application,
  and image cache; it never opens raw seat or framebuffer devices.
- `app/usr/Makefile`: add the image viewer binary.
- `tests/gui/gui-imageviewer.test`: a `GUI-VISUAL-TEST:` fixture that opens a
  known BMP and PNG, zooms/pans, and checks the screen.

Build and run:

```sh
make app
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-imageviewer.test tests/gui
```

Checkpoint:

- BMP and PNG open correctly.
- Malformed headers, truncated rows, oversized BMP dimensions, unsupported PNG
  layout, and libpng decode errors fail visibly without leaking a surface or
  evicting the currently displayed image.
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

- [ ] **M7.1.1 — create a file-manager protocol client and bounded directory
  model.** Connect to `mario-wm`, create a client window, and build typed
  entries from `readdir()` with explicit name/count limits and sorted display
  data. Enable the bounded `LAUNCH_FILE_MANAGER` path form. Unit test long
  names, unknown types, and empty directories. Commit: `filemanager: read
  directory entries`.
- [ ] **M7.1.2 — render a directory listing.** Add name/type/size/time columns,
  selection, and deterministic clipping. Test one listing golden in a
  `GUI-VISUAL-TEST:` fixture. Commit: `filemanager: render directory listing`.
- [ ] **M7.1.3 — navigate paths.** Add parent, selected-directory, and absolute
  path entry navigation; retain the previous listing after an error. Test
  MarioFS, ext2, and vfat without mutating images. Commit: `filemanager:
  navigate filesystem paths`.
- [ ] **M7.2.1 — create and rename in `/tmp`.** Implement mkdir and same-filesystem
  rename with input validation and post-operation refresh. Test success and
  duplicate-name errors. Commit: `filemanager: create and rename files`.
- [ ] **M7.2.2 — copy and move in `/tmp`.** Add bounded streaming copy and
  cross-filesystem move fallback; do not overwrite without confirmation. Test
  contents, error cleanup, and confirmation cancellation. Commit: `filemanager:
  copy and move files`.
- [ ] **M7.2.3 — delete safely in `/tmp`.** Add file delete and explicit
  recursive-directory confirmation without following symlinks. Test nested
  trees, cancellation, and refresh. Commit: `filemanager: delete scratch
  files`.
- [ ] **M7.3.1 — dispatch text and images.** Quote arguments, choose terminal
  versus image viewer, send a bounded server launch request, and retain the
  manager after a launch error. Enable `LAUNCH_OPEN_TEXT` and test both paths.
  Commit: `filemanager: open text and image files`.
- [ ] **M7.3.2 — dispatch executables and refresh.** Launch an executable in a
  terminal through the server, observe client exit, and refresh the originating
  listing. Enable `LAUNCH_EXECUTABLE` and test the complete path in a `/tmp`
  fixture. Commit: `filemanager: launch executables`.

### Detailed Specification

Goal:

- List and navigate directories.
- Open, copy, move, rename, delete, and create directory entries.
- Launch the terminal and image viewer from the file manager.

Change:

- `app/usr/filemanager/`: a `mario-wm` protocol client for directory listing,
  navigation, file operations, and launch logic; it never opens raw seat or
  framebuffer devices.
- `app/usr/Makefile`: add the file manager binary.
- `tests/gui/gui-filemanager.test`: a `GUI-VISUAL-TEST:` fixture that navigates
  `/tmp`, creates files, renames, copies, deletes, and launches an image.

Build and run:

```sh
make app
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-filemanager.test tests/gui
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

## M8: Window-System Hardening

M3 already establishes the multi-process architecture. M8 deliberately adds
no client migration or replacement compositor; it hardens the same server and
protocol after terminal, image-viewer, and file-manager clients are stable.

- [ ] **M8.1.1 — enforce protocol quotas under load.** Stress independent
  input/output/staging limits with all application clients connected. Test
  fair progress, motion coalescing, and deterministic over-quota disconnect.
  Commit: `gui: harden protocol quotas`.
- [ ] **M8.1.2 — recover partial client transactions.** Disconnect clients at
  every protocol and surface-transfer boundary. Verify staging is discarded,
  the previous committed frame remains visible, and no server allocation leaks.
  Commit: `gui: recover partial client frames`.
- [ ] **M8.2.1 — restart the server safely.** Test server crash/restart with
  shell and application clients present; remove stale socket/status files and
  return to a usable text VT before a new session starts. Commit: `gui: recover
  window manager restart`.
- [ ] **M8.2.2 — harden seat and PTY cleanup.** Test F1/F8 during client exit,
  terminal child exit, and repeated shell relaunch; prove no raw device or PTY
  descriptor survives in a client. Commit: `test: cover window system cleanup`.
- [ ] **M8.3.1 — add long-run visual recovery coverage.** Repeatedly create,
  move, resize, close, relaunch, and switch VTs while comparing stable final
  goldens. Commit: `test: stress window manager recovery`.

### Detailed Specification

Goal:

- Harden the M3 server/protocol implementation against load, malformed client
  lifecycles, and recovery paths without changing the visible client model.
- Use Mario's existing pathname `AF_UNIX/SOCK_STREAM` transport; do not add a
  second socket implementation.

Change:

- `app/gui/libmario-ui/protocol.c`: bounded length-prefixed message encode/decode,
  including fragmented-read reassembly, one serialized nonblocking send queue
  per endpoint, `POLLOUT` continuation, and `EPROTO` handling.
- `app/gui/libmario-ui/include/gui/protocol.h`: protocol version, complete
  message enum, and packed payload structs matching the IPC contract,
  including mandatory `HELLO`, server-owned `WINDOW_CONFIGURE`, and
  `SURFACE_BEGIN`/row-ordered `SURFACE_DATA`/`SURFACE_COMMIT` tagged with their
  configure and frame serials.
- `mario-wm`: enforce the protocol's checked wire-length calculation,
  independent input/output byte limits, dynamic scanout-based surface/staging
  budgets, atomic committed-surface rule, and ordered `INPUT_RESET` delivery
  on focus/grab/seat transitions before accepting untrusted apps.
- `app/gui/desktop/`: retain one server and shell-client implementation while
  adding restart/cleanup instrumentation only in unit-test links.
- `app/usr/terminal/`, `app/usr/imageviewer/`, `app/usr/filemanager/`: retain
  their existing protocol-client paths while adding disconnect and relaunch
  fixtures.
- `rd/copy_files.py` and `app/system/init/main.c`: retain `mario-wm` as the
  selected session binary and harden stale socket/status cleanup.
- `tests/gui/gui-mp.test`: a `GUI-VISUAL-TEST:` fixture that exercises
  concurrent clients, partial transfers, server restart, and cleanup rather
  than a first migration.

Build and run:

```sh
make app
make disk-image
make quick-run-gtk
.venv/bin/python tests/lit_gui_test.py --test gui-mp.test tests/gui
```

Checkpoint:

- Applications have run as separate processes since M3.
- Only `mario-wm` opens `/dev/fb0`; clients never receive the framebuffer fd.
- An ordinary-client disconnect removes its windows and releases the PTY/input
  resources; a shell disconnect follows the M3 snapshot-and-one-relaunch rule.
- Protocol unit tests cover fragmented messages, malicious dimensions, all
  allocation/input/output quotas, gapped or duplicate data rows, stale serials,
  partial-update disconnects, incompatible `HELLO` versions, invalid UTF-8
  titles, short-send `POLLOUT` continuation, focus-reset ordering, superseded
  window-configure serials, initial-window placeholder behavior, destroy during
  staging, content-local input coordinates, and only rendering a chunked
  surface after its matching `SURFACE_COMMIT`.
- No single-process desktop fallback exists; a server failure recovers to text
  and a restarted server starts fresh clients.

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
gui: harden window manager recovery and protocol limits
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
make disk-image
make quick-run-gtk
```

M4 uses its focused tmux gate. After every GUI-facing change, also run the
affected text regression; it boots the same image and stays on a text VT.

Targeted tests:

```sh
.venv/bin/python tests/lit_gui_test.py --test fb.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-desktop.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-terminal.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-imageviewer.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-filemanager.test tests/gui
.venv/bin/python tests/lit_gui_test.py --test gui-font.test tests/gui
.venv/bin/python tests/lit_shell_test.py tests/shell/pty.test
```

GUI-runner and input-harness details:

- GUI tests use a fresh QEMU instance with a serial control channel and a QMP
  Unix socket. They start on the initial text console, enter graphics with F8,
  and finish by returning to text. The runner owns QMP's
  `send-key`, `input-send-event`, and `screendump` syntax; test files use the
  stable `GUI-SEND-KEY`, `GUI-TYPE`, `GUI-MOUSE-MOVE`, and
  `GUI-MOUSE-BUTTON` directives. M0/M1 already provide
  `GUI-CHECK-SCREEN: golden.ppm` for a full golden and
  `GUI-CHECK-RECT: x y width height red green blue` for a solid-color probe;
  M1.11 adds crop and screenshot-difference assertions.
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
  `SERIAL-WAIT-FILE: /tmp/mario-wm.ready`; the window manager creates the file
  only after that F8-triggered first full composition and removes it on every
  switch back to a text VT. On startup, the window manager removes stale status
  files; a missing frame file is treated as frame zero.
- `mario-wm` atomically rewrites `/tmp/mario-wm.frame` with a
  monotonically increasing decimal frame number after every completed
  composite. The runner snapshots the prior number before injecting an action
  and waits for a larger one; an abandoned `-EBUSY` flush does not update it. Add
  `GUI-WAIT-FRAME`, which waits through the serial console until that number
  advances; use it after each input action before taking a screendump. Never
  use `SLEEP:` to synchronize a GUI test.
- M1 implements these synchronization directives in `tests/lit_gui_test.py`
  before M3 starts. Remove the runner's fixed screen-settle sleep for desktop
  tests; a missing readiness file or frame advance is a test failure with the
  serial and QMP diagnostics attached.
- Add `GUI-MOUSE-MOVE: dx,dy` and `GUI-MOUSE-BUTTON: button,state` directives.
  The runner owns the QEMU HMP/QMP spelling, so test files describe only Mario
  input actions. Tests inject real PS/2/QEMU events and never write directly
  to `/dev/gui` or `/dev/psaux`.
- Add `GUI-CHECK-SCREEN-RECT: golden.ppm,x,y,w,h` for arbitrary pixel crops;
  it compares the selected screendump crop to the same rectangle in the golden.
  This differs from the existing `GUI-CHECK-RECT`, which checks one rectangle
  for a constant RGB value. GUI PPM comparisons are pixel-exact at
  1024x768x32. The pinned theme, fonts, static `--:--` clock, fixed cursor,
  and deterministic fixtures make a tolerance unnecessary; a changed golden
  requires explicit review.
- After M1.11.2, a fixture with `GUI-VISUAL-TEST:` must contain a screenshot
  assertion; `GUI-CHECK-DIFFERENT` and `GUI-CHECK-MATCH` count because they
  compare QMP screendumps. The runner permits unmarked raw QMP device/ABI tests
  in `tests/gui/` without a screenshot, including tests for framebuffer and
  input records.
- Split coverage by layer: raw QMP GUI tests cover framebuffer/input record
  ABIs and device error paths; marked M3-and-later tests cover visible desktop
  behavior and drive the real server, clients, widgets, PTYs, and applications
  end to end.
- Keep one focused visual golden per behavior: `gui-desktop.test` covers mode
  switching and retained desktop state; `gui-window.test` covers focus, move,
  resize, and close; `gui-terminal.test` covers PTY resize, direct RGB, and
  tmux panes; `gui-imageviewer.test` covers BMP/PNG zoom and pan; and
  `gui-filemanager.test` covers real `/tmp` mutations plus launch behavior.
- On failure, retain the actual screendump and serial tail. If
  `/tmp/mario-wm.debug` exists, also append its serial output; if it is
  absent, record that fact without masking the original failure. M3's debug
  file reports foreground mode, frame number, focused window, pointer
  position, and the last 32 GUI events.
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
- Keep the server/client GUI shippable while protocol quotas and recovery are
  hardened; recover to text on server failure rather than introducing a second
  rendering path. The underlying `AF_UNIX` transport is already covered by its
  own socket and tmux regressions.
- `/dev/fb0` mmap requires physical-device VMA support and must not be
  required by the first GUI. The first implementation uses read/write/pwrite.
- TTF glyph caches can grow quickly at many point sizes. Cap cache entries and
  evict least-recently-used glyphs rather than caching every codepoint.
