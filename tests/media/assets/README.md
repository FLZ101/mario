# Mario media fixtures

These fixtures support the audio-first [mpv port plan](../../../doc/MPV_PORT_PLAN.md).
Start with `stereo-markers.wav` for PCM driver bring-up, then use the AAC MP4s
to test decoding and resampling, and `flash-beep.mp4` to check A/V sync. The two
trailer excerpts are for manual playback checks.

The assets are already generated. FFmpeg is only needed to regenerate them;
it is not a Mario build or runtime dependency for using these files. The default
disk images do not include these fixtures.

| Asset | Purpose |
| --- | --- |
| `big-buck-bunny.mp4` | Manual playback demo with motion and a soundtrack. |
| `sintel.mp4` | Manual playback demo with dialogue and a soundtrack. |
| `stereo-markers.wav` | Exact PCM reference: 48 kHz, signed 16-bit little-endian stereo. Four half-second segments: left only (440 Hz), right only (660 Hz), silence, then both. |
| `stereo-markers-44100.mp4`, `stereo-markers-48000.mp4` | The same markers encoded as stereo AAC-LC in audio-only MP4 containers, for testing resampling, seek and EOF. Compare channel activity and marker timing with tolerance, not PCM bytes. |
| `flash-beep.mp4` | A four-second H.264/AAC-LC clip with white flashes and 880 Hz beeps starting at 1, 2 and 3 seconds, each lasting 0.2 seconds. For A/V sync checks. |
| `flash-beep-drift.mp4` | A 32-second moving 320×180, 15 fps clip with the same pulses once per second, a white guard bar, and a 9-bit black/white frame number. For measuring drift and counting frames actually delivered to the framebuffer. |
| `video-markers.mp4` | Video-only 320×180 H.264 clip: black until 1 s, red until 5 s, then blue until 8 s. For native video output, pause, seek and resize tests. |

The two demo clips are 12-second excerpts starting at 00:10 of the original
trailers, resized and letterboxed to 320×180, 15 fps, H.264 Baseline/YUV420p
with 48 kHz stereo AAC-LC. The flash/beep fixture uses the same encoding format.
The synthetic fixtures do not contain EBU material.

## Sources and Attribution

- **Big Buck Bunny** — (c) copyright 2008, Blender Foundation / www.bigbuckbunny.org.
  [Original trailer](https://download.blender.org/peach/trailer/trailer_480p.mov);
  [project attribution](https://peach.blender.org/about/).
- **Sintel** — © copyright Blender Foundation | durian.blender.org.
  [Original trailer](https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4);
  [project attribution](https://durian.blender.org/sharing/).

Both trailer excerpts are adapted under [CC BY 3.0](https://creativecommons.org/licenses/by/3.0/).
The other fixtures were generated for Mario.

## Regenerate

Run these commands from the repository root. You need `curl`, Bash, and a host
FFmpeg build with the `libx264` and `aac` encoders. Check your FFmpeg first:

```sh
ffmpeg -hide_banner -h encoder=libx264
ffmpeg -hide_banner -h encoder=aac
```

If either encoder is missing, use a different FFmpeg build and set `FFMPEG` to
its executable path. The generator does not install or build FFmpeg.

Download the source trailers to a temporary directory and regenerate the assets:

```sh
sources=$(mktemp -d)
curl -fL https://download.blender.org/peach/trailer/trailer_480p.mov \
    -o "$sources/trailer_480p.mov" &&
curl -fL https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4 \
    -o "$sources/sintel_trailer-480p.mp4" &&
bash tests/media/assets/generate.sh \
    "$sources/trailer_480p.mov" "$sources/sintel_trailer-480p.mp4"
```

For a non-default encoder binary, prefix the `bash` command with
`FFMPEG=/path/to/ffmpeg`. The script overwrites all eight assets next to itself.
Encoded bytes may vary with FFmpeg and encoder versions. The downloaded
trailers are source material only; do not add them to the repository.
