# M9 Integration Audit

This audit resumes M9.6 after the M10 drawing checkpoint. The November 13,
2026 deferrals remain historical evidence, not passing tests. Completion
requires the original three deferred fixtures, the new fullscreen tmux and
sixteen-cycle multiwindow recovery fixtures, and the complete SMP=16 suite.

## Demonstrated Prerequisite Defects

The first resumed `--run-skipped` run on November 14, 2026 preserves the
original workload and records:

- `terminal-tmux-apps.test`: passes in 35.70s, including the original editor
  crops, saved edits and application reclamation.
- `gui-stress.test`: passes all sixteen cycles in 535.23s.
- `wm-many-windows.test`: fails after 589.99s at action 1515, source line 1532.
  The completed-frame counter remains 1087 while the fixture clicks a disabled
  taskbar paging control. This is not an incomplete compositor frame.

The capacity fixture retains every pointer/button action, workload, existing
pixel/focus assertion and deadline. Six redundant requests for a new frame
after paging has reached its boundary become strict disabled-control pixel
checks. No-op input does not require damage or a new scanout; changing that
production invariant would contradict M10's zero-work contract.

The new fullscreen tmux fixture separately reproduces startup starvation.
GDB observes a tmux client repeatedly returning `EAGAIN` from `sendmsg`, while
its server is runnable on the same AP with an unchanged scheduling counter.
APs have no local timer tick in the existing SMP bring-up implementation.
Moreover, the BSP scheduler refills counters for every task whenever its own
queue exhausts a quantum. Frequent serial-shell forks can reset an AP's
remaining quantum before it expires, even when scheduling ticks are forwarded.

The prerequisite fix forwards the existing BSP PIT cadence through a dedicated
scheduler-tick IPI. Each AP decrements its own current task's quantum; normal
interrupt/syscall return still postpones scheduling until a safe userspace
return, preserving kernel spinlock rules. Quantum replenishment affects only
the exhausting CPU's runnable queue, not other CPUs. This does not introduce
a calibrated APIC timer, IO-APIC routing, new socket queues or a drawing ABI.

`smp-preempt.test` parks a worker on a bootstrap pipe before assigning its AP
affinity. Eight pairs of same-CPU busy peers exchange nonblocking pipe
handshakes without sleeping, while the BSP repeatedly forks/reaps short
children to exhaust its own quanta. Each handshake has a five-second deadline,
all child statuses are checked, and pipe descriptors are reclaimed. The
single-CPU fallback tests the same behavior on the BSP. Initial harness
attempts involving `raise`, raw stopped-child status and continuation were
discarded rather than changing unrelated signal or mapping contracts.

The historical Vim SIGTRAP, tmux poisoned pointer and missing-executable
reports are not established to share the starvation cause. A subsequent
fullscreen split-window hang exposes a separate task-lifetime defect:
`sys_waitpid()` scans children without `tasklist_lock` and reaps zombies
without checking their existing `parked` flag. An AP can still be executing
`do_exit()` or its final context switch when a polling parent on another CPU
frees its task and kernel stack. The retained GDB snapshot shows kernel
page-fault exit recursion through poisoned task/session state and other CPUs
blocked on the task-list and scheduler locks.

The prerequisite fix serializes child inspection and unlinking with the
task-list lock, requires a parked zombie, and frees detached tasks outside the
lock. The context-switch tail captures exit state before releasing the
scheduler lock, then notifies and publishes parking under the task-list lock;
no previous-task pointer is accessed after parking becomes visible. Blocking
waiters publish their sleep state before dropping the scan lock, preserving
the wakeup handoff. `smp-reap.test` repeatedly wakes an AP-affine child while
its BSP parent polls `waitpid(WNOHANG)`, requiring all 128 exact exit statuses
within per-child deadlines. Repeated restored fixtures must pass before the
integration checkpoint is marked complete.

The before-fix reap watch catches this exact violation: the BSP enters
`pages_free()` for PID 39, address `0x80878000`, with `parked == 0` while
`cpu_info[1].current_task` still points at it. The AP backtrace is inside
`schedule()` from `do_exit()`. This is a live-stack free, not an inference
from an editor crash. The original uninstrumented regression also times out.

## Fullscreen Tmux Contract

`terminal-tmux-fullscreen.test` enlarges a real terminal to the original
18x44 resized fixture geometry, enters fullscreen, and starts real Vim, top
and nano panes. All three remain alive through restore and fullscreen reentry.
For every configure it checks the terminal PTY with `stty size`, tmux client
dimensions and all pane widths, heights and offsets:

| Mode | PTY / Client | Vim | Top | Nano |
| --- | --- | --- | --- | --- |
| Fullscreen | 46 rows x 124 columns | 62x45 at 0,0 | 61x22 at 63,0 | 61x22 at 63,23 |
| Restored | 18 rows x 44 columns | 22x17 at 0,0 | 21x8 at 23,0 | 21x8 at 23,9 |

The isolated fixture has one terminal and a fully reclaimed initial PTY pool;
its client occupies `/dev/pts/0`. This explicit slave path avoids depending
on the current port's empty tmux `client_tty` string. Pane geometry and
application readiness are queried, not inferred from fixed sleeps.

The new fullscreen reference transplants the original reviewed Vim/nano
buffer-line and active-border pixels to their fullscreen cell coordinates.
Only those three deterministic rectangles are compared; no top statistics,
editor status text or unreviewed full-screen snapshot becomes a golden.
The original restored Vim/nano/border crops remain exact. Both editors save
their inserted lines and retain their original content. All three pane
commands, tmux and the terminal must exit cleanly; original WM/shell identity,
descriptor counts, PTY capacity and the saved text VT must survive.

The captured tmux PTY stream contains vertical absolute positioning (`CSI d`),
erase characters (`CSI X`) and scroll-up (`CSI S`) commands that the existing
parser ignores. These controls are especially visible during fullscreen pane
redraws and restore. Production support preserves the cursor and clips erase
counts to the current line and scroll counts to the active margins. Native
parser assertions cover split input, default/zero counts, oversized counts,
unchanged neighboring cells and cursor preservation. Original goldens remain
unchanged.

The remaining fullscreen corruption reproduces in a host replay of that same
PTY transcript: the parser immediately wraps after writing the rightmost cell.
A top redraw that fills its last row therefore scrolls the full-width margin,
shifting the adjacent Vim pane before its next explicit cursor positioning.
Wrapping now waits for the next printable character; positioning and control
movement cancel pending wrap, while rendition changes preserve it. Saved and
alternate-screen cursors retain pending wrap; resize clears it. Native tests
cover a split edge write, rendition, next-character wrap, bottom-margin redraw,
CR/LF and saved-cursor restoration. The original eager-wrap cursor expectation
is corrected in both basic edge cases without changing any cells or screenshot
golden.

## Multiwindow Recovery Contract

`wm-capacity-stress.test` repeats sixteen capacity, focused-key, fullscreen,
restore and close cycles. Each cycle has fourteen 320x192 windows on one
connection, one independent healthy protocol peer and two real terminals.
Window identities increase across cycles; a destroyed identity is rejected.
The target's one printable key is ordered and reaches no healthy peer. Exact
normal/fullscreen/reveal pixels are checked; every terminal's recorded exit
status, WM/shell identity, descriptor baseline and PTY pool must recover.
The reviewed idle desktop and saved text VT are retained after reclamation.

The full-surface capacity client keeps at most four encoded data messages
outstanding, below the terminal's existing 16 KiB upload bound, and validates
every ordered acknowledgement without changing pixels or deadlines. A rejected
operation on the just-destroyed healthy identity is an explicit protocol
barrier before publishing the cycle's completion marker. This prevents terminal
close shortcuts from racing the healthy peer's asynchronous destruction and
fallback-focus transition.

The new peer harness uses short `/tmp/cap-term.*` names. Its original status
and release basenames exceed Mariofs's filename-component limit and fail with
`ENAMETOOLONG` after both terminal processes have already exited. This is a
fixture-path defect, not a missing terminal exit or a relaxed reclamation check.

Native coverage separately repeats fourteen 96x64 sibling windows and an
independent cached peer across sixteen fullscreen/restore/disconnect cycles.
Every cached pixel, restored geometry, exact per-peer/session metadata and
surface charge, and closed descriptor is checked. Healthy-peer baselines
survive each cycle and teardown returns all charges to zero. The original
40-window/40-peer and bounded large-surface fixtures remain independent gates.

## Validation Ledger

Integration validation is complete. On November 14, 2026, with the SMP
scheduler tick forwarding, task reap handoff, TMUX VT-parser controls, and
serial-paced fullscreen fixture, the focused and previously skipped gates pass
at SMP=16:

- `terminal-tmux-fullscreen.test`
- `wm-capacity-stress.test`
- `terminal-tmux-apps.test`
- `gui-stress.test`
- `wm-many-windows.test`
- `wm-fullscreen.test`
- `smp-preempt.test`, `smp-reap.test`, `ahci-completion.test`,
  `terminal-vt.test`, and `gui-wm-busy.test`

Development logs under `/tmp/mario-m9-*` retain unsuccessful fixture/harness
attempts and diagnostic GDB observations; these are not counted as passing
gates. The original fixture assertions, pixel checks, process-lifetime checks,
and reclamation deadlines remain unchanged.
