# v0.1.0 — First accepted release

Watara Supervision LCD-bus capture to a responsive Windows x64 viewer via
Raspberry Pi Pico and WinUSB. This release preserves the accepted image and
blackout fix without incorporating the later blurry GDI-worker test or the
DXGI experiment that did not improve perceived motion.

## Assets

- `supervision-usb-video-v0.1.0-windows-x64.zip`: accepted viewer, matching Pico
  firmware, instructions, licenses and checksums.
- `supervision-usb-video-v0.1.0-source.zip`: matching preserved source snapshot
  plus release documentation and licensing notices, without local recordings
  or build output.
- `SHA256SUMS.txt`: release asset checksums, including standalone binary hashes.

In the Windows package, launch `SupervisionViewer.exe`. For a new Pico, flash
`SupervisionCapture.uf2` using BOOTSEL. See README for wiring and WinUSB setup.
Existing working installations do not need reflashing simply because the
release files have new package names.

## Accepted binary identities

`SupervisionViewer.exe` is a byte-identical copy of
`SupervisionViewer_TEST_NO_BLACK_CLEAR_20260930.exe`:

`162C5DCB9648B1A73063B12F71A8577CCD1A66DB6EFE5CC3CBB76ED14CB3895B`

`SupervisionCapture.uf2` is a byte-identical copy of
`TEST_VIDEO_FIRST_LINE_FAST_HANDOFF_20260930.uf2`:

`EC7F5DE0546866548243D49D213384FE20589A36E503BA5ABCDE98C80A796CF6`

The accepted source was preserved before the DXGI/GDI-worker experiments.
Only release documents, notices and packaging helpers are added. Compilation
verification is separate from these preserved binary assets; rebuilt files
are not substituted for the tested binaries.

## Known limitations and disclosure

Uneven paddle motion and temporal mixing at moving edges remain. No claim of
perfect smoothness, universal hardware compatibility or 100% physical-LCD
equivalence. No motion interpolation or audio capture. Approximately 50.815
raw pairs/s measured on the tested console is not monitor refresh or a
guarantee for every device.

The project was developed in large part with AI assistance (OpenAI ChatGPT
and Codex), with human-directed hardware work, testing and release selection.

Original contributions are MIT; derived capture firmware retains DutchMaker's
non-commercial license. Forking/improvements must respect that restriction.
See `THIRD_PARTY_NOTICES.md`; the entire release is not unrestricted MIT.
