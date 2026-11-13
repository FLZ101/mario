# GUI Drawing Baseline

This is the test-only M10.1 baseline for [M10](roadmap.md#m10-incremental-gui-drawing),
not an implementation of incremental drawing. Production composition, scanout,
terminal rendering, and the wire protocol are unchanged. M10.2–M10.8 and the
deferred M9 integration checkpoint remain separate work items.

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

All presented cases currently clear/compose and flush the entire 1024x768
screen. A full flush uses 768 writer calls, each accepting 4096 bytes. The
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

## Validation Record

The November 14, 2026 final run completed successfully:

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

## Validation Boundaries

The recorded `GUI-TMUX-STARTUP`, `GUI-STRESS-CLEANUP`, and `GUI-CAPACITY-FRAME`
deferrals remain explicit skips; no new skip is introduced. A passing runnable
suite does not establish that those integrations pass or complete M9.6/M10.
GTK dual-cursor/alignment behavior is not changed or measured by this headless
QMP benchmark. Keep the same compiler flags, host/accelerator, resolution, SMP,
instrumentation, and sample policy for the M10.8 comparison.
