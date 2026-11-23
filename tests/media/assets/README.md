# Mario media fixtures

These fixtures support the audio-first [mpv port plan](../../../doc/MPV_PORT_PLAN.md).
Start with `stereo-markers.wav` for PCM driver bring-up, then use the AAC MP4s
to test decoding and resampling, and `flash-beep.mp4` to check A/V sync. The two
trailer excerpts are for manual playback checks.

The assets are already generated. FFmpeg is only needed to regenerate them;
it is not a Mario build or runtime dependency for using these files. The
standard and GTK disk images include `stereo-markers.wav` and all three Big
Buck Bunny variants in `/usr/share/mario-tests/media`. From a Mario desktop
terminal on a default QEMU boot, run:

```sh
mpv /usr/share/mario-tests/media/big-buck-bunny.mp4
mpv /usr/share/mario-tests/media/big-buck-bunny-720p.mp4
mpv /usr/share/mario-tests/media/big-buck-bunny-1080p.mp4
```

The packaged player takes only the filename and requires a usable `/dev/pcm0`
for clips with AAC audio. It does not drop frames, so higher resolutions can
play slower than real time.

| Asset | Purpose |
| --- | --- |
| `big-buck-bunny.mp4`, `big-buck-bunny-720p.mp4`, `big-buck-bunny-1080p.mp4` | The same playback demo at 320×180, 1280×720, and 1920×1080. |
| `sintel.mp4` | Manual playback demo with dialogue and a soundtrack. |
| `stereo-markers.wav` | Exact PCM reference: 48 kHz, signed 16-bit little-endian stereo. Four half-second segments: left only (440 Hz), right only (660 Hz), silence, then both. |
| `stereo-markers-44100.mp4`, `stereo-markers-48000.mp4` | The same markers encoded as stereo AAC-LC in audio-only MP4 containers, for testing resampling, seek and EOF. Compare channel activity and marker timing with tolerance, not PCM bytes. |
| `flash-beep.mp4` | A four-second H.264/AAC-LC clip with white flashes and 880 Hz beeps starting at 1, 2 and 3 seconds, each lasting 0.2 seconds. For A/V sync checks. |
| `flash-beep-drift.mp4` | A 32-second moving 320×180, 15 fps clip with the same pulses once per second, a white guard bar, and a 9-bit black/white frame number. For measuring drift and counting frames actually delivered to the framebuffer. |
| `video-markers.mp4` | Video-only 320×180 H.264 clip: black until 1 s, red until 5 s, then blue until 8 s. For native video output, pause, seek and resize tests. |
| `video-markers-1080p.mp4` | Twelve numbered 1920×1080 frames at 30 fps, H.264 Baseline/YUV420p, no audio. The first six frames have a blue band and the rest have a red band. For checking zero-drop windowed and maximized playback. |

The demos are 12-second excerpts starting at 00:10 of the original trailers,
scaled and letterboxed as needed. They use 15 fps H.264 Baseline/YUV420p with
48 kHz stereo AAC-LC. The flash/beep fixture uses the same encoding format.
The synthetic fixtures do not contain EBU material.

## Sources and Attribution

- **Big Buck Bunny** — (c) copyright 2008, Blender Foundation / www.bigbuckbunny.org.
  [Original 1080p trailer](https://download.blender.org/peach/trailer/trailer_1080p.mov);
  [project attribution](https://peach.blender.org/about/). The image includes
  [the attribution notice](big-buck-bunny-attribution.txt) next to the clips.
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
curl -fL https://download.blender.org/peach/trailer/trailer_1080p.mov \
    -o "$sources/trailer_1080p.mov" &&
curl -fL https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4 \
    -o "$sources/sintel_trailer-480p.mp4" &&
bash tests/media/assets/generate.sh \
    "$sources/trailer_1080p.mov" "$sources/sintel_trailer-480p.mp4"
```

For a non-default encoder binary, prefix the `bash` command with
`FFMPEG=/path/to/ffmpeg`. The script overwrites the assets next to itself.
Encoded bytes may vary with FFmpeg and encoder versions. The downloaded
trailers are source material only; do not add them to the repository.
