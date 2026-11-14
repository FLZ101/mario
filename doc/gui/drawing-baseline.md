# GUI Drawing Baseline

This preserves the test-only M10.1 baseline and the M10.8 comparison for
[M10](roadmap.md#m10-incremental-gui-drawing). M10.2–M10.7 implement incremental
composition, scanout, terminal rendering, and negotiated uploads. The deferred
M9 integration checkpoint remains separate from the runnable drawing gate.

## Reproduce

Use the common 1024x768 VBE image and the declared `-O0` build. Ensure existing
artifacts actually use those flags; merely changing a make variable does not
invalidate every previously compiled object. Do not run other QEMU tests during
the timing capture. The artifact lock protects image consistency, not host CPU
isolation.

```sh
make app OPTIMIZE=-O0
make disk-image-vbe_1024x768
.venv/bin/python tests/test_gui_drawing_benchmark.py
.venv/bin/python tests/lit_shell_test.py --smp 16 --test gui-wm-busy.test tests/shell
.venv/bin/python tests/lit_gui_test.py --smp 16 --test gui-drawing-performance.test tests/gui
.venv/bin/python tests/bench_gui_drawing.py --smp 16 --output /tmp/mario-gui-baseline.json
make test SMP=16
git diff --check
```

The JSON includes revision/dirty paths, source and binary SHA-256 fingerprints,
the ramdisk fingerprint, resolution, declared optimization, QEMU version and
command, effective KVM/TCG selection, SMP, CPU model/affinity, memory, host load
at setup and completion, instrumentation, workload, and sampling policy. The
serial log shares the output stem. A failed run also retains a screendump and
guest diagnostics and exits nonzero. Preserve the original JSON and serial
log when collecting `/tmp/mario-gui-after.json` later.

## Deterministic Work

`gui-wm-busy` includes the existing WM source in its test-only link. Its wrappers
count clipped fills and blits, logical flush requests, calls to the real
toolkit flush routine's writer callback, bytes accepted by that callback, and
completed-frame advances independently. The writer accepts complete requests;
the count is not a syscall trace of an interrupted device. Existing allocation,
transaction, publication-reader, fullscreen, and suspended-scanout assertions
remain enabled. Additional clipping checks prevent counting offscreen work.

Fills/blits count pixel stores, including overdraw, not unique screen pixels.
The deterministic fixture has transparent cursor storage: direct opaque sprite
stores are not included in these primitive counts. The QMP fixture and timing
workloads use the real server-owned cursor and verify its visible movement.
Client glyph/cell rendering and upload bytes are not inferred from WM counters;
their deterministic instrumentation belongs to subsequent terminal items.

All presented baseline cases clear/compose and flush the entire 1024x768
screen. A baseline full flush uses 768 writer calls, each accepting 4096 bytes. The
ordinary window's content is 320x192; the move is eight pixels to the right.

| Case | Filled Pixels | Blitted Pixels | Flush Requests | Writer Calls | Written Bytes | Completed Presentations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Idle, unchanged pointer | 0 | 0 | 0 | 0 | 0 | 0 |
| Cursor, +32 pixels | 926324 | 847872 | 1 | 768 | 3145728 | 1 |
| Small window move | 926324 | 847872 | 1 | 768 | 3145728 | 1 |
| Visible window commit | 926324 | 847872 | 1 | 768 | 3145728 | 1 |
| Fully hidden window commit | 0 | 0 | 0 | 0 | 0 | 0 |
| Fullscreen window commit | 1572864 | 786432 | 1 | 768 | 3145728 | 1 |

The ordinary-case filled count is the screen clear plus decorated bounds,
title, close button, and content clear. Its blitted count is desktop plus
committed content. Fullscreen skips the obscured desktop and ordinary windows,
but still clears the screen and fullscreen content before copying the cache.
These are exact assertions for the pre-incremental path, not M10's final bounds.

### Final Deterministic Work

The original workloads and counter meanings are retained. Their final counts
are hard assertions, not estimated savings from smaller framebuffer writes:

| Case | Filled Pixels | Blitted Pixels | Flush Requests | Writer Calls | Written Bytes | Completed Presentations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Idle, unchanged pointer | 0 | 0 | 0 | 0 | 0 | 0 |
| Cursor, +32 pixels | 512 | 512 | 2 | 32 | 2048 | 1 |
| Small window move | 211832 | 133380 | 1 | 218 | 287760 | 1 |
| Visible window commit | 184320 | 122880 | 1 | 192 | 245760 | 1 |
| Fully hidden window commit | 0 | 0 | 0 | 0 | 0 | 0 |
| Fullscreen window commit | 1572864 | 786432 | 1 | 1 | 3145728 | 1 |

Cursor scanout in this fixture is the sum of two disjoint 16x16 footprints,
2048 bytes, versus 3145728 bytes before. The general region helper may merge
touching/overlapping footprints into their bounding box: at most 32x32 pixels
(4096 bytes) for two 16x16 sprites. Disjoint footprints remain separate. This
documents the bounded coalescing overhead rather than promising their exact
union in every direction. Opaque sprite/blend stores add at most 256 stores
per presentation, independently of the clipped fill/blit counters. When a
cursor intersects windows, primitive work also counts those damaged scene
intersections and their overdraw; unrelated screen pixels are not composed.
These cursor-only bounds apply to settled scanout; initial presentation and
foreground/session recovery still require a full-screen repaint.

The eight-pixel drag flushes its 330x218 old/new decorated bounding rectangle,
287760 bytes, not the whole screen. Narrow scanout remains scanline-based;
the full-width layout-safe replacement batches 768 rows into one writer call.
The full-screen content still needs a full repaint; batching does not claim
to eliminate its pixel work or bypass framebuffer generation checks.

The final native fixture repeats idle, hidden update, motion, overlap/reveal,
and close/reopen with sixteen charged 320x192 windows. A sixteen-record
motion-only burst preserves final position and presents exactly once at its
deadline, with 2048 scanout bytes. Its small drag retains the same 287760-byte
ceiling; fills are bounded by nine damaged-region areas (647460 stores), and
blits by three (215820 stores), covering the desktop and the two intersecting
visible window layers, not sixteen full-screen draws. Each of sixteen
close/reopen cycles restores surface/record charges and client count, proves
the closed descriptor is invalid, preserves every healthy peer pixel, and
matches the full-scene oracle. Cleanup returns all native charges to zero.

The renderer oracle covers unchanged state, glyph overhang, cursor movement,
scrolling, alternate screens, erases, direct RGB, Unicode and resize rollback.
Its single-cell bound is a 24x32 dirty rectangle, 2336 counted stores and at
most twelve glyph calls; unchanged state performs no drawing. The real `A`
with adjacent cursor fixture produces a 16x16 patch with exactly 1140 wire
bytes and 1024 scanout bytes, preserving all pixels outside the rectangle.
WM primitive bounds there are 768 fills and 256 blits. Distant dirty cells and
scrolling may coalesce into much larger rectangles; they are not single-cell
traffic. Four encoded unacknowledged chunks peak at 16144 bytes; the encoder
is 4036 bytes, reply and PTY-input queues are 4096 bytes each, and PTY-output
scratch is 512 bytes. Delayed/short/stale/error replies never expand these
caps or expose a mixed frame.

## Timing Method

Each of six cases uses two warm-ups followed by eight timed samples, spaced
150 ms apart outside the timed interval. Mouse cases alternate +32/-32 pixels
as either one event or sixteen two-pixel events. Window cases hold the title
bar drag across samples; the real terminal content remains 320x192.

Terminal cases inject Enter into a real PTY shell. Each request emits 512
65-byte lines (33280 payload bytes), then repaints the grid in an alternating
RGB color. Guest Python creates the fixture in setup; a guest byte-count probe
must confirm its exact size before timing starts. The normal and fullscreen
cases use the same payload. Fullscreen setup waits for visible committed
geometry and completed-frame publication before injecting another request,
rather than assuming a fixed sleep completes the configure transaction.

The clock starts immediately before QMP injection and stops when a screendump
first satisfies the final-state predicate. It includes QMP command/round-trip
and screendump overhead and the 10 ms polling cadence; it is an observational
upper bound, not a device-only latency measurement. Multi-event samples measure
the whole burst through its final visible position, not individual event
latencies. Boot, readiness, payload generation, fullscreen setup, initial
screenshots, image-lock waits, and inter-sample spacing are excluded.

Statistics use the median, nearest-rank p95, and maximum. With eight timed
samples, p95 equals the maximum. Throughput is payload bytes divided by
injection-to-final-marker latency: terminal rendering, scrolling, and the final
RGB repaint are included in elapsed time but repaint/control bytes are not in
the numerator. It is not sustained transport throughput or an FPS claim.

Warm-ups are reported separately. Failures retain their index, warm-up status,
error, and elapsed time when injection began. A failed/stale sample aborts the
case and the run; requested sample counts remain recorded and zero successful
samples produce null statistics, not a zero-latency success.

## Recorded Run — November 14, 2026

The uncontended QEMU capture started at `2026-11-14T03:27:29.271184+08:00`,
after the other test guests exited. Revision was
`56494aaa5b2777b135ef1cf594790f073d054dde` with the M10.1 working-tree changes;
this is not a clean-commit measurement. The complete report is
`/tmp/mario-gui-baseline.json`, with `/tmp/mario-gui-baseline.serial.log`.

- Host: WSL2, Linux `5.10.16.3-microsoft-standard-WSL2`, x86_64, glibc 2.39.
- CPU: Intel Core i9-13900H, 20 logical CPUs, affinity 0–19.
- Host memory: 26618015744 bytes; ambient host load is not controlled.
- Load averages at setup: 9.066 / 5.851 / 2.897; at completion: 3.870 / 4.892 / 2.773.
- Compiler: GCC `16.2.1 20261029 (Red Hat 16.2.1-3)` in the local Fedora 45
  build container, `-O0`, static i386 userspace.
- QEMU: `8.2.2`, package `Debian 1:8.2.2+ds-0ubuntu1.12`, default TCG,
  KVM disabled, 16 guest CPUs, 256 MiB guest memory, `-vga std`, headless QMP.
- Scanout: 1024x768x32, 4096-byte pitch. All six work-counter cases match
  the exact table above; all 12 warm-ups and 48 timed samples succeed.

| Workload | Timed Samples | Median (ms) | p95 (ms) | Max (ms) | Failures |
| --- | ---: | ---: | ---: | ---: | ---: |
| Cursor, one event | 8 | 156.593 | 181.936 | 181.936 | 0 |
| Cursor, sixteen events | 8 | 515.971 | 791.206 | 791.206 | 0 |
| Window drag, one event | 8 | 110.598 | 150.483 | 150.483 | 0 |
| Window drag, sixteen events | 8 | 681.164 | 823.722 | 823.722 | 0 |
| Terminal normal burst | 8 | 544.172 | 862.319 | 862.319 | 0 |
| Terminal fullscreen burst | 8 | 2398.802 | 4668.178 | 4668.178 | 0 |

| Terminal Workload | Payload Bytes | Median (bytes/s) | p95 (bytes/s) | Max (bytes/s) |
| --- | ---: | ---: | ---: | ---: |
| Normal burst | 33280 | 61659.666 | 102863.605 | 102863.605 |
| Fullscreen burst | 33280 | 13874.526 | 19516.136 | 19516.136 |

Throughput percentiles are calculated from each sample's individual byte rate,
not by inverting the latency percentile. Rounded timed samples, in order, are
retained here as well as at full precision in the JSON:

```text
milliseconds
cursor-1-events:          178.814 181.936 145.375 135.342 38.670 167.811 41.950 178.652
cursor-16-events:         499.732 479.032 532.210 488.203 538.905 571.467 492.348 791.206
window-drag-1-events:     112.330 114.518 52.530 133.002 105.613 108.865 37.229 150.483
window-drag-16-events:    614.822 753.768 746.100 513.842 763.586 823.722 359.948 616.228
terminal-normal-burst:   385.402 323.535 401.101 593.300 862.319 687.825 495.044 630.882
terminal-fullscreen-burst: 4668.178 3706.089 2351.381 2379.146 2418.457 2337.181 2797.353 1705.256
```

The measured binary/ramdisk SHA-256 fingerprints are:

```text
mario-wm.exe:    ae0229113dbe3b2312d5dd5fa3c8407d13007673a8e75482ea6d64e2e9130078
terminal.exe:    b426d2a044d99ca5f3c6f2e09845f698a87c0550275ce73e8c8714741d0ba11c
gui-wm-busy.exe: 98390299bd231dff206881dfdf8b3f926efac8c470e2abc6c56cf7a5386281b5
rd/rdx:         7989146b001478d17bcd273b6aa8ba6214faba4fd902af465ff174a7b490ec36
```

## Development Diagnostics

The initial smoke run timed out during fullscreen setup: Enter was injected
while the server still suppressed ordinary input for its pending configure.
Its report is `/tmp/mario-gui-smoke.json`. Payload validation also rejected two
guest-shell generators producing 65 and 32769 bytes, preserved in
`/tmp/mario-gui-smoke-fixed.json` and `/tmp/mario-gui-smoke-verified.json`.
These setup failures are not silently treated as valid 33280-byte throughput
samples. The corrected smoke run is `/tmp/mario-gui-smoke-payload.json`.

The first full-suite run found the new visual fixture's terminal PID was set
inside a serial heredoc subprocess and unavailable to its parent shell's
`wait`. Launch now runs in the parent serial shell; the existing clean-exit
assertion remains intact and its focused visual test passes.

## After Capture — November 14, 2026

The uncontended after capture starts at `2026-11-14T08:11:24.083015+08:00`,
after the focused recovery guest exits. Revision is
`42efd7cee8dc38f97b9dd271252ce82c58cd7f43` with the M10.8 test/documentation
changes; like the baseline, this is a working-tree measurement. Production
WM/terminal binaries contain M10.2–M10.7. The full report and serial transcript
are `/tmp/mario-gui-after.json` and `/tmp/mario-gui-after.serial.log`.

Host, CPU affinity, memory, Fedora 45 GCC 16 build, `-O0`, QEMU 8.2.2 TCG,
16 guest CPUs, 256 MiB, 1024x768x32, workload, polling and sampling policy
match the baseline. The benchmark script itself is unchanged. All twelve
warm-ups and 48 timed samples succeed; there are no discarded/failed samples
or report errors. The six final work-counter records match the hard table.

Ambient host load is not controlled: setup/completion load averages are
0.062 / 0.170 / 0.486 and 0.196 / 0.191 / 0.478, respectively, substantially
lower than the earlier baseline. Neither capture runs alongside another QEMU
guest, but these observations are not an isolated causal estimate of the
code change. The deterministic work reductions are independent of host load;
timings retain the stated QMP/screendump/polling overhead and imply no universal
hardware FPS target.

| Workload | Baseline Median (ms) | After Median (ms) | After p95 (ms) | After Max (ms) | Observed Median Ratio | Failures |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Cursor, one event | 156.593 | 29.178 | 45.384 | 45.384 | 5.367x | 0 |
| Cursor, sixteen events | 515.971 | 33.514 | 35.503 | 35.503 | 15.396x | 0 |
| Window drag, one event | 110.598 | 42.510 | 44.883 | 44.883 | 2.602x | 0 |
| Window drag, sixteen events | 681.164 | 30.629 | 47.118 | 47.118 | 22.239x | 0 |
| Terminal normal burst | 544.172 | 237.187 | 279.500 | 279.500 | 2.294x | 0 |
| Terminal fullscreen burst | 2398.802 | 1160.784 | 1271.825 | 1271.825 | 2.067x | 0 |

Each row has eight timed samples. Cursor/drag sixteen-event timings are not
sixteen-window timing measurements; the independent sixteen-window fixture
provides deterministic work/recovery coverage instead.

| Terminal Workload | Payload Bytes | Baseline Median (bytes/s) | After Median (bytes/s) | After p95 (bytes/s) | After Max (bytes/s) |
| --- | ---: | ---: | ---: | ---: | ---: |
| Normal burst | 33280 | 61659.666 | 140442.048 | 193999.392 | 193999.392 |
| Fullscreen burst | 33280 | 13874.526 | 28678.159 | 31250.304 | 31250.304 |

Per-sample throughput, not an inverted latency percentile, supplies the rates.
Rounded after samples are retained alongside the full-precision JSON:

```text
milliseconds
cursor-1-events:          28.571 28.636 45.384 29.720 43.926 27.796 31.552 27.881
cursor-16-events:         30.007 35.503 33.987 33.492 33.242 33.275 33.537 35.159
window-drag-1-events:     43.212 41.681 42.660 29.207 43.858 42.361 32.521 44.883
window-drag-16-events:    28.289 47.118 30.854 30.153 29.453 34.758 30.403 33.273
terminal-normal-burst:  171.547 197.832 219.507 229.951 244.423 254.297 279.500 272.886
terminal-fullscreen-burst: 1064.950 1100.992 1088.104 1141.548 1180.019 1210.240 1271.825 1257.356
```

After binary/ramdisk SHA-256 fingerprints:

```text
mario-wm.exe:    354d88eafb466433f84623172b74b303ca33d99ca2b7a6910dbc161b82ab946f
terminal.exe:    f8a4796e7dbb344ea7ee900292011d0147e2369568e311d3ac329a7120ffcde9
gui-wm-busy.exe: 014237005804d24a2836e96174e584b6a8a64a2d8d64497956f79ef2e06a437e
rd/rdx:         ad1ca8b85d440e5eab6b838870b0f523058da5e8152970d2f427a4de6a0a3b71
```

## Recovery Audit

`gui-drawing-performance.test` exercises one terminal's exact-color content and
drag/reveal, then sixteen independent terminals. It verifies final input to
the focused peer, untouched healthy peer input, a real 46x124 fullscreen PTY
and 10x36 restored PTY, exact RGB pixels before/after restore and close/reveal,
sixteen clean closes, and sixteen subsequent EOF/close/reopen cycles. WM/shell
PIDs and descriptor baselines survive; the recorded PTY pool is fully restored.
The native sixteen-window test separately verifies exact surface/metadata
charges and every healthy cached pixel on each close/reopen, then zero charges
at teardown. Native delayed-reply tests and the repeated timed payloads cover
continuous output and resize/cancellation without buffer growth.

The unchanged runnable suite retains `wm-damage`, `wm-motion-batch`,
`wm-occlusion`, `wm-fullscreen`, `wm-recovery`, framebuffer transition/generation
and flush tests, real terminal input/resize/RGB, plain tmux, and Vim/top/less
editing/quit/reclamation fixtures. These retain reviewed pixels, foreground
F1/F6/F8 return, exact restore geometry, replacement-shell recovery, sibling
health and old allocation/scanout-failure assertions. No golden is replaced
and no comparison tolerance is introduced by M10.8.

The user-approved `GUI-TMUX-STARTUP`, `GUI-STRESS-CLEANUP` and
`GUI-CAPACITY-FRAME` exclusions remain explicit and unchanged. The original
resized tmux Vim/top/nano fixture, M9.6 fullscreen tmux pane/edit audit, and
original multiple-windows-per-connection capacity/stress scenarios are **not**
claimed to pass. The new independent-terminal drawing/recovery fixture does
not silently replace those deferred contracts. M10 drawing work and its
runnable gate are therefore reported separately from unqualified M9/M10
integration completion.

Development runs `/tmp/mario-m10-8-performance*.log` retain fixture failures.
The first version waited for an oksh WINCH trap while its builtin read blocked;
geometry is now queried by real input after each completed configure. The
second version ignored read EOF and sampled PTY capacity after an earlier
sleeping child, making the baseline transient. The first child now exits
through a marker, the baseline precedes every workload, and reader EOF ends
the child. Recorded exit-status wrappers stay alive until the parent releases
and reaps them, avoiding lost historical shell job statuses without accepting
nonzero application exits. A short-child cycle's missing exit marker is also
retained, not dropped. The final focused native, visual and benchmark-host
tests pass; timing capture itself has no failed sample.

## Baseline Validation Record

The M10.1 baseline's November 14, 2026 final run completed successfully:

- Focused `gui-wm-busy.test`: passes, including exact counters and clipping.
- Focused `gui-drawing-performance.test`: passes with the real desktop,
  terminal, pointer, drag/reverse, and clean-exit assertions.
- Benchmark: six counter cases, 12 successful warm-ups, 48 successful timed
  samples, zero failures; verified 33280-byte terminal payloads.
- Host tests: 12 benchmark tests and all existing image-builder, pexpect,
  GUI-parser, and skip-parser tests pass (49 tests in the full-suite run).
- `make test SMP=16`: exits zero, with 77 shell, six TTY, and 62 GUI cases
  passing (145 guest cases). Two SMP-4-only shell cases are not applicable
  at SMP 16; the three documented GUI deferrals remain explicit skips.
- `git diff --check`: passes. Normal kernel/image artifacts are restored
  after the framebuffer self-test variants.

The full-suite log is `/tmp/mario-m10-final-suite.log`. Focused logs are
`/tmp/mario-m10-busy-focused.log` and
`/tmp/mario-m10-drawing-final-focused.log`. M10.1 passes its implementation
gates; the recorded timings identify the working tree before that step's commit.

## Final M10.8 Validation Record

The November 14, 2026 final drawing/recovery gate completes successfully:

- Focused `gui-wm-busy.test`: passes in 19.29s, including sixteen-window work
  bounds, exact cached pixels and reclaimed resource charges.
- Focused `gui-drawing-performance.test`: passes in 86.06s, including sixteen
  real terminals, fullscreen/restore, ordered input, clean closes and repeated
  capacity recovery. The same fixture passes in 86.50s in the full gate.
- After benchmark: twelve warm-ups and 48 timed samples succeed, no report
  errors or discarded failures; sampling, workload and benchmark script unchanged.
- Host benchmark and GUI-parser tests pass; all 49 host tests pass in the full gate.
- `make test SMP=16`: exits zero with 77 shell, six TTY and 68 GUI cases
  (151 guest cases), using the default four workers and the same five explicit skips.
- `git diff --check`: passes; the suite restores normal kernel/image artifacts
  after its framebuffer self-test variants.

Final logs are `/tmp/mario-m10-8-suite.log`, `/tmp/mario-m10-8-native-final.log`,
`/tmp/mario-m10-8-performance4.log`, `/tmp/mario-m10-8-benchmark.log`,
`/tmp/mario-m10-8-benchmark-unit.log`, and `/tmp/mario-m10-8-parser-final.log`.
The timing comparison and recovery audit above qualify what this gate proves;
the deferred integrations remain separate.

## Validation Boundaries

The recorded `GUI-TMUX-STARTUP`, `GUI-STRESS-CLEANUP`, and `GUI-CAPACITY-FRAME`
deferrals remain explicit skips; no new skip is introduced. A passing runnable
suite does not establish that those integrations pass or complete unqualified
M9/M10 integration, including M9.6.
GTK dual-cursor/alignment behavior is not changed or measured by this headless
QMP benchmark. The M10.8 comparison retains the same compiler flags,
host/accelerator, resolution, SMP, instrumentation and sample policy, with
the explicitly reported ambient-host-load difference.
