# GUI Test Fixtures

Golden screenshots are complete 1024 by 768 PPM QMP screendumps checked by
`GUI-CHECK-SCREEN`:

- `fb.ppm`: raw `/dev/fb0` write result.
- `fb-tty.ppm`: framebuffer text-terminal state.
- `fb-tty-cursor-hidden.ppm`: framebuffer text terminal with the cursor hidden.

Other files are deterministic source fixtures, not golden screenshots:

- `checker-64.bmp`: 64 by 64 24-bit BMP with four repeating colors; verifies
  BMP channel order and bottom-up decoding.
- `alpha-128.png`: 128 by 128 RGBA PNG; verifies PNG decode and alpha blend.
- `large-1600x1200.png`: RGB PNG larger than the baseline framebuffer; verifies
  decoder downscaling, fit mode, and pan behavior.
- `files/`: immutable source tree copied into guest `/tmp/gui-fixtures/` before
  file-manager mutation tests.

SHA-256:

```text
d0c414a8c058c42b44a4918286ebcb8aca05194db344f07338d5e320df48d7f9  checker-64.bmp
8db9fc5f353bc8f96337922e912d62fe1d1ed5580d5db9261985226b8b4a493c  alpha-128.png
ca467fd8fb71a0486be862f8ca2ffaebe574fca7c2671a5fd3c9851a0415ab71  large-1600x1200.png
```
