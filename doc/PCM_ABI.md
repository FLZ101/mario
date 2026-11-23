# Mario PCM playback

Mario automatically probes PCI High Definition Audio controllers at boot. The
driver selects the first usable analog line-out or headphone path. Opening
`/dev/pcm0` returns `ENODEV` without a suitable controller and codec.

## Interface

Open `/dev/pcm0` for writing. A second independent writer gets `EBUSY`; a
duplicate or inherited descriptor shares ownership. The last close discards
pending PCM and stops DMA. Data is signed 16-bit little-endian stereo at
48,000 frames/s, interleaved left then right. A frame is four bytes.

The [ABI header](../src/include/uapi/mario/pcm.h) defines the controls and
version-1 status. `write` accepts whole frames only (`EINVAL` for an unaligned
length). It can return an aligned partial count. A blocking write waits only
if no frame fits and can be interrupted by a signal; a nonblocking write with
no space returns `EAGAIN`. `poll(POLLOUT)` reports at least one writable frame.
Writes neither start nor restart DMA.

| Control | Effect |
| --- | --- |
| `MARIO_PCM_START` | Start queued PCM from stopped or underrun state; returns `EAGAIN` if empty. |
| `MARIO_PCM_PAUSE` | Request pause. Already-staged DMA data plays out; unstaged PCM remains queued. Wait for `PAUSED` in status. |
| `MARIO_PCM_RESUME` | Start queued PCM from `PAUSED`, using a freshly primed stream. |
| `MARIO_PCM_DRAIN` | Stop after queued data and a silent tail. Wait for `STOPPED`; writes report `EBUSY` while draining. |
| `MARIO_PCM_RESET` | Stop and discard data immediately; clear progress and underrun counters. |
| `MARIO_PCM_STATUS` | Snapshot state, capacity, writable and pending frames, consumed frames, and underrun count. |

Pending and consumed count valid frames at the latest DMA poll, not frames
already audible at the jack. A partially filled descriptor reserves a whole
period until it is retired. Pause and drain allow a 60 ms silent tail after
DMA transfers the last valid frame. Reset and close do not wait for audio
already buffered in the codec or host sink.

The driver polls roughly every 10 ms. A 35 ms service gap or implausible DMA
position stops the stream and discards unaccountable PCM rather than guessing
at a wrap. This cannot undo audio the hardware has already emitted: a stall
longer than a full ring can replay data before software can stop it. Validate
the polling deadline under load on the reference PC. If that deadline cannot be
met, add completion interrupt servicing.

## Validation

Build the guest test and images with `make kernel app` and `make disk-image`.
Run the following with `N=1` and `N=2`; logs and WAV captures are retained in a
unique temporary directory. No FFmpeg or host audio device is required.

```sh
.venv/bin/python tests/media/hda_test.py --mode device --smp "$N"
.venv/bin/python tests/media/hda_test.py --mode no-codec --smp "$N"
.venv/bin/python tests/media/hda_test.py --mode no-controller --smp "$N"
.venv/bin/python tests/media/hda_test.py --mode late --smp "$N"
```

For the manual listening check, run
`.venv/bin/python tests/media/hda_test.py --mode device --listen pa` (or
`--listen pipewire`). The GTK QEMU run should play left, right, silence, and
both channels, followed by two short markers. An intentional pause falls
inside the first marker.

For the physical gate, boot a Mario-compatible PC and run
`/usr/bin/test/pcm.exe`. Use another task to read from the reference disk
throughout. Confirm the analog jack, log the HDA controller and codec path
from `dmesg`, and follow the cold-boot/reboot and recovery checklist in the
[port plan](MPV_PORT_PLAN.md#m2h--physical-pc-audio-gate).
Physical playback has not yet been verified.
