# Desktop Exit to `tty1` When Running Fullscreen `btop`

## Symptom

Booting with `make run-gtk SMP=8`, switching to the desktop (`F8`), opening
`Terminal` from `Menu`, toggling fullscreen (`Super+F`), and running `btop`
worked normally at first. After roughly 80–90 seconds, the terminal and desktop
suddenly vanished and the display dropped back to `tty1`.

## Investigation

### 1. Headless reproduction matching `make run-gtk SMP=8`

To observe guest serial state, process lifetimes, open descriptors, and disk
usage while driving the GUI, we booted QEMU with the exact `run-gtk` disk set
(`qemu/hd1-gtk.img`, `qemu/hd2.img`, `qemu/hd3-gtk.img`), `-smp 8`,
`-machine vmport=on`, NAT `e1000`, `ich9-intel-hda`, `-serial stdio`, and a QMP
socket:

1. Wait for `/tmp/mario-wm.started` and `mario-desktop: private connection ready`
   in `/tmp/mario-wm.log`.
2. Send `F8` over QMP to enter the desktop.
3. Click `Menu` -> `Terminal`, press `Super+F` (`meta_l,f`) for fullscreen, and
   run `btop`.
4. Periodically sample `pgrep`, `/proc/<pid>/fd`, `/tmp/mario-wm.log`,
   `/tmp/mario-wm.frame`, and `df -k /` over the serial shell.

### 2. Observing the failure at ~85 seconds

Polling the guest every few seconds exposed a steady monotonic growth in
`/tmp/mario-wm.log` and a corresponding drop in free blocks on the root
`mariofs` ramdisk (`/`):

- The 2.5 MiB root ramdisk (`rd0-gtk`, 5120 blocks of 512 bytes) starts with
  roughly ~780 KiB–1 MiB of free space.
- Fullscreen `btop` (128×48 cells) redraws truecolor ANSI escapes and Braille
  graph glyphs at ~10–15 KiB/s.
- `/tmp/mario-wm.log` grew at that exact ~10–15 KiB/s rate until `/` hit `0`
  free blocks (`ENOSPC`, `errno = 28`) at ~80–86 seconds.
- Inspecting `/proc/<terminal_pid>/fd` showed:
  - `fd 0 -> /dev/console`
  - `fd 1 -> /tmp/mario-wm.log`
  - `fd 2 -> /tmp/mario-wm.log`
- Inspecting the tail of `/tmp/mario-wm.log` confirmed it was filled with raw
  `btop` ANSI/Braille escape sequences followed by:
  ```text
  mario-wm: frame publication failed: 28
  ```

### 3. Tracing the root-cause chain in the source

Five behaviors combined to turn `btop`'s terminal output into a full desktop
teardown:

1. **`init` redirects `mario-wm` stdout/stderr to `/tmp/mario-wm.log` on `/`:**
   In `app/system/init/main.c` (`spawn_window_manager()`), `init` opens
   `/tmp/mario-wm.log` (`O_WRONLY | O_CREAT | O_TRUNC`) and `dup2`s it onto
   `STDOUT_FILENO` and `STDERR_FILENO` (leaving `STDIN_FILENO` on
   `/dev/console`).
2. **`mario-wm` forked ordinary client applications without detaching stdio:**
   `wm_start_application()`, `wm_start_terminal_command()`, and
   `wm_start_terminal_executable()` in `app/gui/desktop/mario-wm/main.c` called
   `fork()` + `execl()` directly, so launcher-started `terminal` inherited
   `STDOUT_FILENO` and `STDERR_FILENO` pointing to `/tmp/mario-wm.log`.
3. **`terminal` echoed every raw PTY byte to its own `STDOUT_FILENO`:**
   `terminal_drain_pty()` in `app/usr/terminal/main.c` contained a leftover
   debug write:
   ```c
   if (count > 0) {
       terminal_vt_feed(vt, buffer, count);
       *updated = 1;
       if (write_full(STDOUT_FILENO, buffer, count) < 0)
           return -1;
       continue;
   }
   ```
   Every frame drawn by `btop` on the PTY slave was mirrored byte-for-byte into
   `/tmp/mario-wm.log` until the root ramdisk ran out of blocks.
4. **`ENOSPC` killed both `terminal` and `mario-wm`:**
   Once `/` had 0 free blocks:
   - `write_full(STDOUT_FILENO, ...)` in `terminal_drain_pty()` failed with
     `ENOSPC` and returned `-1`, breaking `terminal`'s main loop and closing
     the terminal window.
   - Compositing that window update (or the next `btop`/cursor frame) called
     `wm_compose()` -> `wm_publish_completed_frame()` -> `write_status_file()`
     to write `/tmp/mario-wm.frame.new` on `/tmp` (also on `/`).
   - Creating/writing `/tmp/mario-wm.frame.new` failed with `ENOSPC` (`28`),
     causing `wm_compose()` to return `GUI_FLUSH_ERROR` and `mario-wm` to jump
     to `out:` and exit.
5. **Releasing the graphics seat switched the console back to `tty1`:**
   When `mario-wm` exited and closed `/dev/fb0` and `/dev/gui`,
   `gui_fb_writer_release()` / `gui_release()` in `src/fs/char/gui.c` detected
   loss of the active seat and called
   `switch_text_console(get_fg_text_console_index())`, returning the screen to
   `tty1`. (`init` then respawned a dormant `mario-wm`, which truncated
   `/tmp/mario-wm.log` back to empty while remaining in text mode until `F8`.)

## Fix

1. **Stop echoing raw PTY bytes from `terminal` (`app/usr/terminal/main.c`):**
   Removed `write_full(STDOUT_FILENO, buffer, count)` from
   `terminal_drain_pty()`. `terminal` still logs concise lifecycle lines
   (`terminal: window ready`, `terminal: child exit %d`) when explicitly run
   with redirected logs, but never mirrors child PTY traffic to stdout.
2. **Detach ordinary client stdio in `mario-wm`
   (`app/gui/desktop/mario-wm/main.c`):**
   Added `wm_detach_client_stdio()` to open `/dev/null` (`O_RDWR`) and `dup2`
   it onto `STDIN_FILENO`, `STDOUT_FILENO`, and `STDERR_FILENO` in the child
   branches of `wm_start_application()`, `wm_start_terminal_command()`, and
   `wm_start_terminal_executable()`. `wm_start_shell()` (`mario-desktop`)
   intentionally keeps `mario-wm`'s log stream for session readiness and
   appearance diagnostics.
3. **Update documentation and tests:**
   - Updated `doc/gui/desktop.md` to document silent PTY draining and
     `/dev/null` stdio for WM-launched ordinary clients.
   - Updated tests that previously relied on `terminal` echoing child PTY bytes
     to its stdout (`app/test/gui-wm-busy/main.c`,
     `app/test/gui-wm-client/main.c`, `app/test/terminal-winsize/main.c`,
     `tests/gui/terminal-apps.test`, and sibling `tests/gui/*.test` fixtures).
   - Added `tests/gui/terminal-launcher-log-flood.test` to launch `Terminal`
     from the desktop menu, assert `/proc/<pid>/fd/{0,1,2}` resolve to
     `/dev/null`, toggle fullscreen (`meta_l,f`), stream ~3 MiB of truecolor
     ANSI output (exceeding the 2.5 MiB root ramdisk), and verify
     `/tmp/mario-wm.log` does not grow and the desktop remains live.

## Verification

```sh
make app
make disk-image-vbe_1024x768
make disk-image-gtk
.venv/bin/python tests/lit_gui_test.py --smp 8 \
  tests/gui/terminal-launcher-log-flood.test \
  tests/gui/terminal-apps.test
```

In addition, re-running the headless `make run-gtk SMP=8` reproduction with
fullscreen `btop` for over 200 seconds confirmed that `mario-wm`,
`mario-desktop`, `terminal`, and `btop` kept constant PIDs, `/tmp/mario-wm.frame`
advanced past 400 frames, `/tmp/mario-wm.log` stayed flat at 96 bytes, `/` free
space stayed constant, and pressing `q` exited `btop` cleanly back to the
fullscreen shell without leaving the desktop.
