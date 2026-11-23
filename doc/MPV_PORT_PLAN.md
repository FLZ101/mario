# Minimal mpv port for Mario

## Scope

Run `mpv /path/to/clip.mp4` from a Mario terminal and play it in a normal desktop
window. Support seekable local MP4 with 8-bit H.264 (YUV420p) and, when present,
AAC-LC audio. Support pause/resume, seek, quit, window close and clean exit at EOF.
**Deliver audio first:** prove PCM playback without mpv, then AAC through mpv
with video disabled, and only then add windowed video. A file with an AAC track
must not silently fall back to null audio if the device cannot open. Start with
320×180 at 15 fps and stereo audio under the existing 256 MiB QEMU configuration.

Target both QEMU and at least one physical x86 desktop that already boots Mario
and exposes a PCI Intel HDA controller with an analog codec and a wired line-out
or headphone jack. Record the exact board, controller, codec, and working jack.
This is not a promise of audio on every PC: HDMI/DisplayPort, USB audio, legacy
AC’97, virtio-snd, and DSP-only devices are out of scope for the first release.
Keep PCM as the decoded sample format and use the same `/dev/pcm0` API for both
virtual and physical HDA; the device name does not imply AC’97.

Skip streaming, playlists, external tracks, subtitles, scripts, JSON IPC, hardware
decoding, GPU rendering and other media formats. Build mpv with the other apps,
but keep disk-image packaging opt-in. Do not introduce SDL, X11 or Wayland just
to open a window.

## Milestone Map

Complete each exit gate before starting the next milestone unless the gate is
explicitly deferred. A build that links or a QEMU command that starts is not
proof of playback. M0 through M1b and the automated M2 QEMU gates are verified.
The M2 listening check and M2H physical-PC gate were deferred to start M3;
both remain outstanding. M3 through M5 pass in QEMU on one and two vCPUs.

| Milestone | Deliverable | Exit gate |
| --- | --- | --- |
| M0 — Fixtures | Existing host-side PCM, AAC, A/V sync, and demo assets | Host decode verifies channel markers and flash/beep timing; done. |
| M1a — PCI/MMIO prerequisite | HDA and AHCI register mappings coexist | Both PCI BARs remain independently usable under concurrent disk and audio probing. |
| M1b — HDA bring-up | One HDA output stream plays known integer PCM | QEMU WAV confirms the expected stereo signal; missing or unsupported audio hardware does not block boot. |
| M2 — PCM device | `/dev/pcm0`, playback controls, and recovery | Captured PCM and guest counters agree through pause, reset, underrun, wrap, and reopen on one and two vCPUs. |
| M2H — Physical PC | Same HDA driver and `/dev/pcm0` on the reference desktop | Analog output passes channel, pause, reset, and reopen checks during disk I/O on the recorded board and codec. |
| M3 — Guest runtime | Verified i386/musl floating-point and wait behavior | Guest x87 state survives scheduling, migration, and signals; runtime probes pass. |
| M4 — Static mpv build | Standard app build with media recipes under `app/usr/video/` | Static ELF32 mpv decodes both AAC fixtures to a diagnostic null output in the guest. |
| M5 — Audible AAC | `ao_mario` plays local MP4 without video | QEMU capture has the correct markers at both input rates after pause/seek and through EOF. |
| M6 — Native video | `vo_mario` renders H.264 in the compositor | GUI test observes distinct frames, controls work, and closing releases the window and PCM device. |
| M7 — A/V sync | Timed and bounded playback at the initial target | Flash/beep skew stays within 100 ms in the reference QEMU run, including after pause/seek. |
| M8 — Opt-in packaging | Local-only `/usr/bin/mpv` and image/test integration | Enabled image plays a demo in QEMU and on the reference PC; the disabled image contains no player or media payload. |

### Validation Rules

- Use the [existing fixture inventory](../tests/media/assets/README.md). Keep
  `generate.sh` beside those assets; do not require host FFmpeg to run tests.
- Run each focused guest gate with `SMP=1` and `SMP=2` once it is stable. Keep
  captures and serial/player logs on failure. Use bounded waits and observable
  state; reserve performance measurements for a controlled run.
- Keep kernel PCM tests independent of mpv throughout. Driver initialization,
  test fixtures and opt-in player packaging must not make ordinary boots or
  test runs depend on host audio. Do not package the player by default at M8.

## M1a — PCI and MMIO Prerequisites

- Pick the reference PC early. From a known-working OS, record PCI device and
  subsystem IDs, codec vendor/revision and analog pin configuration, and verify
  that the target jack produces stereo audio. Prefer a desktop line-out over a
  laptop speaker path that may require machine-specific amplifier setup.
- Reuse Mario's [PCI helpers](../src/include/pci.h#L6). Enumerate HDA class
  `04:03` candidates rather than binding blindly to the first function: an HDMI
  controller may appear first. Validate the firmware-assigned memory BAR and
  reject addresses outside Mario's supported physical address range. Enable
  memory decoding and bus mastering without clearing unrelated PCI state or
  writing back status bits.
- Extend the [MMIO helper](../src/mm/ioremap.c) so an HDA BAR
  can coexist with AHCI. Use uncached/device mappings, preserve byte offsets,
  check address/size overflow, and unwind partial allocation failures. Check
  kernel mapping visibility and lifetime on SMP; failure to map or unmap HDA
  must not alter the AHCI mapping. Keep this a separately reviewable change.

**Exit:** Boot QEMU with both AHCI and HDA, read stable HDA capability registers,
and verify disk I/O before and after an HDA probe failure. Repeat on one and two
vCPUs. No audio ABI is introduced here.

Verified on QEMU 9.2.4 with `python3 tests/media/hda_test.py --smp 1 --bridge`
and `python3 tests/media/hda_test.py --smp 2`. Two HDA functions passed stable
register reads and unmap checks after AHCI disk discovery; the AHCI exact-sector
test passed afterward. MMIO map mutations are confined to the boot CPU before
AP startup. The `0xff400000` aperture keeps device PTEs out of the RAM direct
map, whose physical limit is now 2036 MiB.

## M1b — HDA DMA and Codec Bring-up

- Use the same driver for the [Intel HDA interface][intel-hda] on the reference
  board and QEMU's [ICH9 controller][qemu-hda] with its output-only codec. The
  installed QEMU accepts the following device/backend combination; this verifies
  device creation, not guest audio:

  ```sh
  -device ich9-intel-hda,id=mario-hda,msi=off \
  -device hda-output,bus=mario-hda.0,audiodev=mario-audio \
  -audiodev "wav,id=mario-audio,path=$capture_wav,out.frequency=48000,out.channels=2,out.format=s16"
  ```

  Keep device/backend selection opt-in and use a real host sink for manual GTK
  testing. Poll initially; do not assume MSI or working PCI interrupt routing.
- Reset the controller with bounded waits, enumerate responding codec addresses,
  and send verbs through one command path (CORB/RIRB with polled responses).
  Validate response codec IDs and handle missing or nonresponsive codecs without
  blocking boot. Walk one analog output path from the audio function group to
  a DAC and line-out/headphone pin via supported selectors or mixers. Use pin
  defaults and capabilities; configure power, connection selection, amps, pin
  control and EAPD only where supported. Do not hardcode QEMU's node IDs for
  physical hardware. Log the selected path and why others were rejected; keep
  any required board quirk scoped to the recorded subsystem and codec.
- Set one output DMA stream and its codec converter to 48 kHz, stereo, S16,
  with matching stream tag/channel and format. Use kernel-owned DMA buffers and
  a buffer descriptor list with spec-compliant alignment; use 32-bit-addressable
  DMA memory for the initial driver. Track byte units (`CBL`, BDL lengths,
  `LPIB`) and convert to stereo frames for userspace. Start with 960-frame
  periods (20 ms, 3,840 bytes) and an 80 ms queue budget. For bring-up, use a
  bounded integer PCM self-test and stop before the cyclic buffer repeats; do
  not add a temporary public ABI.
- Add a focused capture runner: use a unique WAV path per QEMU run and quit
  QEMU cleanly to finalize its header. Check decoded samples and channel order,
  not just file size or a guest success message. Record the QEMU backend and
  settings so M2 and M7 can reproduce the run.

**Exit:** The WAV confirms the expected stereo signal. The same kernel still
boots with no audio controller or usable codec. No mpv or floating point is
needed for this milestone.

Verified with `python3 tests/media/hda_test.py --mode pcm --smp N` for `N=1,2`
on QEMU 9.2.4. The WAV backend ran at 48 kHz, stereo, S16 with a 1 ms audio
timer. The capture contains exactly 480 left-only frames followed by 480
right-only frames, with the expected polarity and no duplicate markers.
The test skips a codec-less controller before selecting a working controller.
The `no-codec` and `no-controller` modes boot and pass the AHCI check at both
CPU counts. The boot self-test is opt-in through `hda=selftest`.

## M2 — PCM Device and Recovery

- Service active playback on Mario's 100 Hz timer initially, without enabling
  device IRQs or busy-spinning. The [current PCI IRQ path](../src/kernel/entry.S#L423)
  is AHCI-specific. Verify sustained playback under disk and GUI load; only add
  shared PCI INTx dispatch if measurements show polling is inadequate. Serialize
  service with producer/control operations. Do not hold a spinlock across user
  copies or sleeps. Quiesce DMA and any pending timer callback before releasing
  or reusing DMA memory, including on close, process exit, and probe failure.

- Expose a Mario-specific `/dev/pcm0` through the character-device table and
  `rd/device_list`. One writer at a time (`EBUSY`); ownership follows the open
  file description, including `dup`/`fork`. No capture, mixing, mmap, runtime
  format negotiation, or ALSA/OSS compatibility. The wire format is interleaved
  signed 16-bit little-endian stereo at 48 kHz (four bytes per frame).
- Use `write` for whole-frame PCM and `poll(POLLOUT)` for space. Specify blocking,
  signal interruption, aligned partial writes and `O_NONBLOCK`/`EAGAIN`. Freeze
  a shared, versioned, fixed-width UAPI only after testing the semantics: explicit
  `START`, `PAUSE`/`RESUME`, `RESET` (discard), and `STATUS`; add a separate drain
  ioctl only if needed. Writes queue without starting a stopped stream. Status
  atomically reports stream state, capacity, writable and pending frames, a
  consumed-frame counter and an underrun indication. Writable capacity must
  account for free descriptors, not just free payload bytes. `RESET` empties the
  queue and resets progress; close discards rather than waiting indefinitely.
  The implemented interface and its limits are in [PCM ABI](PCM_ABI.md).
- Treat the HDA BDL as cyclic, not an AC’97-style appendable descriptor queue.
  Keep software-queued valid data separate from the cyclic DMA buffer. Reuse
  only safe regions and ensure starvation cannot play old data on wrap. Track
  wrap-aware position using `LPIB` initially; compare the DMA position buffer
  if real hardware reports unreliable positions. A missed poll can make a
  modulo position ambiguous; force delayed servicing in tests and treat lost
  progress as an underrun, not a successful wrap. If polling cannot prevent
  stale replay under load, require interrupt-driven servicing before shipping.
  Document hardware FIFO and partial-period behavior. Count only valid PCM,
  not any silence needed to fill a final period, and verify short EOF.
- Stop or re-prime on underrun and reset. Verify the controller has stopped
  before reclaiming DMA. Do not assume toggling `RUN` preserves mid-period
  position on every target; preserve and re-prime pending samples or pause at
  a verified boundary if needed. Test that pause/resume skips or repeats no
  valid audio and bound any latency or tail this strategy introduces.
- Define latency honestly: pending device frames count audio not yet transferred
  by HDA DMA, not samples still buffered in the codec, QEMU, or the host sink.
  Measure that residual latency in M7; do not infer it from writes or a guest
  wall clock.

- Add an integer-only guest test program in `app/test` that plays generated PCM
  markers with distinct left/right patterns, independent of FFmpeg and x87. Use
  `stereo-markers.wav` as the fixed reference for format, channel order, marker
  order, and sample counts. Allow for leading/trailing capture silence; use
  longer marked sequences for wrap and stress cases.
- Cover descriptor wrap, short final periods, partial/nonblocking I/O, pause in
  a period, reset while queued, underrun and restart, close/reopen, competing
  writers, missing hardware, and sustained playback with AHCI I/O on one and
  two vCPUs. Check that an interrupted blocked writer can exit and that closing
  one duplicate fd does not release ownership prematurely.

**Exit:** Guest counters and host capture agree, including pause continuity and
no replay of discarded guest PCM after reset. Confirm audible output manually in
GTK before moving to the physical machine.

The `device`, `late`, `no-codec`, and `no-controller` test modes exercise the
automated portion at both CPU counts. `device` compares the captured fixture
and short markers with guest progress while a child performs exact-sector AHCI
reads. `late` forces a missed poll and verifies that it reports an underrun
rather than crediting unobserved DMA progress. GTK/PulseAudio smoke tests also
pass at both CPU counts. These runs do not replace a human listening check or
the physical analog test.

## M2H — Physical-PC Audio Gate

- Boot the same HDA driver on the reference board and use the integer PCM
  marker test through `/dev/pcm0`. Capture the analog jack with an independent
  recorder if available; compare channel order, tone identity and duration, not
  byte equality. Record whether cold boot and reboot produce the same codec
  route and audible output. Record the board, PCI/subsystem IDs, codec/pin,
  firmware settings, and kernel revision with the result.
- Verify pause/resume, short EOF, underrun/restart, and reset/reopen while
  running disk I/O; repeat on SMP if the reference board supports it in Mario.
  Ensure silence after reset/exit and verify disk reads still work. If a codec
  quirk is necessary, keep a QEMU regression and record the evidence for the
  narrowly scoped board fix. Defer other boards and output paths rather than
  claiming generic PC support from one analog codec.

**Exit:** Audible playback from the recorded analog jack is repeatable. A QEMU
WAV capture alone does not satisfy this gate; without compatible hardware,
physical support remains unverified.

## M3 — Guest Runtime Prerequisites

- The guest probes check x87 registers and the control word across task switches,
  `fork`, `exec`, nested signals, and explicit CPU migration. The kernel saves
  x87 state before allowing the outgoing task to run on another CPU. SIMD is
  out of scope for bring-up; disabling FFmpeg assembly alone does not prevent
  compiler-emitted floating-point instructions.
- The probes also cover monotonic clocks, pthread wait timeout and wakeup,
  `poll` with no descriptors, `ppoll` timeout and pipe wakeup, file seeks and
  EOF, and AF_UNIX stream socket I/O. Run with `N=1` and `N=2` after building
  the kernel, applications, and disk images:

  ```sh
  .venv/bin/python tests/lit_shell_test.py --smp "$N" --jobs 1 \
    tests/shell/media-runtime.test tests/shell/media-runtime-migrate.test
  ```

**Exit:** Focused guest tests exercise the required behavior on one and two vCPUs,
including migration on SMP. Fix or explicitly route around any missing runtime
behavior before depending on it from the player.

## M4 — Static Cross-Build and Decode

- `make app` builds mpv alongside the other apps. The recipes in `app/usr/video/`
  pin and verify [mpv 0.41.0][mpv-build], [FFmpeg 7.1.1][ffmpeg], and
  [libplacebo 6.338.2][placebo]. The build package lists provide `pkg-config`.
  The build venv installs Meson (at least 1.3), Ninja, and Jinja2 from
  `requirements.txt`. Meson uses Ninja from that venv, which also runs the
  Python generators. No host FFmpeg executable or development libraries are used.
- FFmpeg builds its six required static libraries with only local file I/O,
  the MOV/MP4 demuxer, AAC decoding and parsing, and minimal audio filtering
  and resampling. Networking, assembly, and SIMD are disabled. M6 adds the
  H.264 decoder and parser to this build.
- libplacebo runs without graphics backends or a target C++ runtime: its numeric
  conversions use C and musl. mpv builds the CLI without libmpv installation,
  upstream graphics backends, or [libass][libass]. Subtitle text rendering and graphical
  text OSD are unavailable in this build; terminal status still works.
- Meson uses Mario's musl wrapper and staged target `pkg-config` metadata, with
  no target binaries executed on the host. The build verifies a static ELF32
  i386 executable with no interpreter or dynamic dependencies before installing
  it to `app/usr/video/install/bin/mpv`.
- Build the normal kernel and disk images as for the shell suite, then run the
  focused check with `N=1` and `N=2`:

  ```sh
  .venv/bin/python tests/media/mpv_decode_test.py --smp "$N"
  ```

  The test uses `mke2fs` to put mpv and the two checked-in AAC fixtures on a
  disposable QEMU disk. It checks decoder selection, the number of stereo
  frames delivered to `--ao=null` (within one AAC frame of two seconds), and
  successful exit at EOF. It leaves normal and GTK images unchanged.

**Exit:** A clean app build yields the same target and feature set without
host library leakage. A development image runs `mpv --no-video --ao=null` on
both AAC fixtures, selects an AAC decoder, produces decoded audio, and exits
at EOF. Keep this diagnostic path out of the normal launch interface.

Verified on one and two QEMU vCPUs: the 44.1 and 48 kHz fixtures delivered
89,088 and 96,256 stereo frames, respectively, and exited successfully at EOF.
The link uses staged media libraries, Mario's musl, and Mario's compiler runtime.

## M5 — mpv Audio Output

- `ao_mario` is an [mpv push backend][mpv-ao] for `/dev/pcm0`. It requests
  48 kHz stereo S16 and lets mpv resample and remap input. It accepts whole
  mpv writes into a staging buffer bounded by the device capacity. Timed
  refill uses nonblocking device writes, retaining any unwritten frames.
  Latency includes both staged and device-pending frames.
- Pause lets already-staged DMA samples play out, then resumes the queued
  audio. Seek discards both queues; closing the player releases the PCM device.
  At natural EOF the backend reports playback active until the device drains.
  It does not fall back to null audio by default if `/dev/pcm0` cannot open.

**Exit:** Play `stereo-markers-44100.mp4` and `stereo-markers-48000.mp4` with
`--no-video --ao=mario`. The capture preserves channel activity and marker
timing within AAC/resampling tolerance, plays the final marker to EOF, and
does not repeat old audio after pause or seek beyond the measured host-buffer
tail. A missing/busy device produces an explicit failure, not silent fallback.

Run the capture gate with `N=1` and `N=2` after building the kernel, apps,
and disk images. It tests both AAC rates, including an exact seek while paused,
and rejects absent and busy PCM devices. QEMU captures 48 kHz stereo PCM without
a host audio device. The WAV includes the transition around pause but does not
record the full wall-clock pause while DMA is stopped.

```sh
.venv/bin/python tests/media/mpv_audio_test.py --smp "$N"
```

## M6 — Native Video Output

- Enable FFmpeg's H.264 decoder and verify a local 8-bit H.264 MP4 with null
  video output before connecting the compositor.
- Add `vo_mario` using `libmario-ui` and the normal window-manager socket. Follow
  [imageviewer's surface upload and window handling](../app/usr/imageviewer/main.c#L293):
  convert with `libswscale` to the GUI's XRGB8888 layout, letterbox into a bounded
  surface, and use `SURFACE_BEGIN` / `SURFACE_DATA` / `SURFACE_COMMIT`. Do not open
  `/dev/fb0` or replace the compositor. Reuse mpv's frame timing and input commands.
- Poll GUI input and mpv's wakeup source together; handle resize and close while
  paused as well as while playing. Keep staging bounded and drop obsolete frames
  when the compositor is busy, respecting configure/frame serials and transaction
  boundaries. Measure full-frame uploads at the target size before proposing a
  new transport or shared memory.

The native VO shares `libmario-ui`'s bounded uploader with the terminal. It
finishes the in-flight transaction and keeps only the newest waiting frame.
Resize/seek cancel stale uploads; a resize rerenders the retained frame even
while paused. `--msg-level=vo/mario=trace` logs conversion-to-COMMIT times, not
scanout timestamps. The full 320×180 RGB surface is 225 KiB before headers.
In a QEMU smoke run of `flash-beep.mp4` through EOF, the player submitted
32 surfaces on one vCPU (120 ms median) and 57 on two (10 ms median). These
are submission counts and coarse guest clock measurements, not A/V sync or
sustained frame-rate guarantees. M7 covers both of those gates.

Build the VBE image with `make disk-image-vbe_1024x768`, then run both checks
with `N=1` and `N=2`:

```sh
.venv/bin/python tests/media/mpv_decode_test.py --smp "$N"
.venv/bin/python tests/media/mpv_video_test.py --smp "$N"
```

The decode check requires all 60 H.264 frames to reach `--vo=null`. The GUI
check uses `mcopy` (from `mtools`) and `sfdisk` to enable HDA on a disposable
copy of the boot disk. It checks actual QEMU screen pixels with the video-only
color markers and the A/V flash fixture. Neither test installs mpv in the
normal or GTK images.

**Exit:** A focused GUI test observes frame changes, a stable paused frame,
redraw on seek/resize, and clean close/EOF. Check both an A/V clip and a video-only
clip; the latter must not claim `/dev/pcm0`. Closing releases the window and lets
another player open the device.

## M7 — A/V Sync and Performance

- Let mpv drive video from its audio clock for files with AAC. Test a clip with
  coincident flashes and tones: correlate completed video presentations with
  host audio capture on a common clock during playback and after pause/seek.
  WAV frame offsets alone do not share a time origin with GUI timestamps;
  record a common capture timing reference before asserting skew. Record
  the backend, buffer settings, and measurement uncertainty with the traces.
- Use `flash-beep.mp4` for sync, then repeat a longer sequence to check for
  drift. Measure residual QEMU/host audio latency, delivered/skipped frames,
  PCM underruns, and peak memory. Keep ordinary GUI regressions based on
  observable state rather than asserting host timing in every CI job.

**Exit:** In the documented QEMU configuration, verify at most 100 ms of A/V
skew, including after pause/resume and seek. Verify sustained 320×180 playback at
15 fps with no audio underruns during steady playback in the 256 MiB guest.
If the gate fails, measure and tune buffers before changing the ABI or claiming
accuracy for other backends.

## M8 — Local-Only Packaging and Regression

- Enforce the local-file scope in the packaged entry point, not only in an mpv
  config file. Accept a single regular `.mp4` file, verify the container and H.264
  stream, and reject URLs, stdin, pipes, devices and playlists. Do not expose
  alternate load paths, external MOV references, or auto-loading of other files.
  Keep the unrestricted headless diagnostic path out of the packaged entry point.
- Stage `/usr/bin/mpv` through [the `/usr` image manifest](../rd/copy_files.py#L182)
  behind an opt-in image setting. Use the GTK overlay for a manual demo clip,
  and the normal test image for automated fixtures so tests do not depend on GTK
  or the wallpaper overlay. Verify that turning the option off removes staged
  artifacts without requiring a clean app build.
- Add shell tests for exit status, decode/seek/EOF and rejection of nonlocal or
  unsupported inputs. Reuse the focused audio and GUI gates; keep the independent
  PCM tests runnable with mpv disabled.

**Exit:** Run the local-only input tests and the focused audio/GUI suites against
the enabled image; manually play a demo under `make run-gtk` and on the recorded
reference PC. Build again with the option off without cleaning first and confirm
the image contains no player or media-only payload, then run the standard
regressions.

[intel-hda]: https://www.intel.com/content/www/us/en/standards/high-definition-audio-specification.html
[qemu-hda]: https://gitlab.com/qemu-project/qemu/-/blob/v9.2.4/hw/audio/intel-hda.c
[mpv-build]: https://github.com/mpv-player/mpv/blob/v0.41.0/meson.build
[mpv-ao]: https://github.com/mpv-player/mpv/blob/v0.41.0/audio/out/internal.h
[ffmpeg]: https://github.com/FFmpeg/FFmpeg/blob/n7.1.1/configure
[placebo]: https://github.com/haasn/libplacebo/blob/v6.338.2/src/meson.build
[libass]: https://github.com/libass/libass/blob/0.17.4/configure.ac
