# GUI Image Fixtures

The image-viewer and wallpaper tests use these immutable files from
`/usr/share/mario-tests/gui/`. Tests never mutate the installed images.

| File | Format | Dimensions | SHA-256 |
| --- | --- | --- | --- |
| `checker-64.bmp` | BMP, 24-bit RGB | 64 × 64 | `d0c414a8c058c42b44a4918286ebcb8aca05194db344f07338d5e320df48d7f9` |
| `alpha-128.png` | PNG, 8-bit RGBA | 128 × 128 | `8db9fc5f353bc8f96337922e912d62fe1d1ed5580d5db9261985226b8b4a493c` |
| `large-1600x1200.png` | PNG, 8-bit RGB | 1600 × 1200 | `ca467fd8fb71a0486be862f8ca2ffaebe574fca7c2671a5fd3c9851a0415ab71` |
| `wallpaper-blue-marble-1920x1200.png` | PNG, 8-bit RGB | 1920 × 1200 | `7d110aece67e4e175717c941e984cfdc5ee4e6f94a69b3a199a8fc84687497e7` |

## Real Wallpaper

`wallpaper-blue-marble-1920x1200.png` is the NASA Blue Marble desktop
wallpaper, converted from the original JPEG to a non-interlaced RGB PNG
without resizing. It is a satellite-derived composite, not a synthetic
test pattern or an AI-generated image.

- Source page: https://science.nasa.gov/resource/blue-marble/
- Original image: https://assets.science.nasa.gov/content/dam/science/esd/climate/2023/12/ImageWall5_1920x1200-80.jpg
- Original JPEG SHA-256: `739d86617a6d82a031979e87caafcfda83fb7fbfbc1f1addfb5955451361e43c`
- Credit: NASA Goddard Space Flight Center; imagery by Reto Stöckli, with
  visualization and compositing by Robert Simmon. Supporting data come from
  the MODIS teams, USGS, and the Defense Meteorological Satellite Program.
- Usage guidance: https://www.nasa.gov/nasa-brand-center/images-and-media/
  The image is included as informational GUI test material. NASA does not
  endorse Mario.

`desktop-wallpaper-real.test` compares the whole 1024 × 768 framebuffer,
including the panel and pointer, rather than checking a few flat-color pixels.
It verifies launcher and terminal overlays, exact background restoration after
both close, and cached rendering after removing the configured image path and switching
between text and graphics modes. No network access is needed to run the test.
The guest loads the image through a temporary symlink, which is removed to
test caching without copying the large photograph into the small root ramdisk.

To generate the host-only reference `desktop-wallpaper-blue-marble.png`, crop
160 pixels from each side of the source, then sample
`(floor(x * 25 / 16), floor(y * 25 / 16))` from that 1600 × 1200 crop to
cover 1024 × 768. Copy the bottom 28 rows from `desktop-default.png`, which
includes the panel and the pointer at `(0, 752)`, outside the launcher button.
Reference SHA-256:
`318280881c40a9077c2508dec08d9689c9d446584d70626cc42f41c0990e16d9`.
