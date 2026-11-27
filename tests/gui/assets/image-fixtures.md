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

`desktop-wallpaper-real.test` compares small, distinctive patches of the scaled
wallpaper beneath the launcher and terminal. It verifies background restoration
after each overlay closes and cached rendering after removing the configured
image path and switching between text and graphics modes. The wallpaper patches
are checked at fixed positions to verify placement. No network access is needed.
The guest loads the image through a temporary symlink, which is removed to
test caching without copying the large photograph into the small root ramdisk.

`desktop-wallpaper-blue-marble.png` is a QEMU screenshot of the desktop
after it renders the wallpaper fixture.

## Screen References

Save each image reference from a single screenshot of the relevant state.
Reuse a screenshot for multiple features when it contains them; do not
assemble crops from different screenshots into a mosaic. Use the smallest
exact rectangle that distinguishes the feature or state being tested.
`GUI-CHECK-SCREEN-RECT: asset,x,y,w,h,anywhere` finds that crop at any screen
position. Keep a fixed rectangle or full frame when placement, occlusion,
damage, or VT restoration is the behavior under test.
Use solid-color `anywhere` checks only for distinctive probe colors, not
for fills that might also appear in unrelated controls.

`GUI-CLICK` and `GUI-RIGHT-CLICK` find a unique crop before clicking it.
`GUI-POINT` locates a control before a hover or drag. In these directives,
the coordinates are in the reference screenshot, not the live framebuffer.
Exclude pointer pixels, clocks, and other live data from the selected
rectangle unless the test is specifically checking them.
