# Release preparation verification — 2026-09-30

The staged source comes from the accepted
`RESPONSIVE_NO_BLACKOUT_20260930` preservation snapshot. Its C/C++/header/PIO,
CMake files and two build scripts are copied without runtime-code edits.
Public-facing documentation, license notices, binary checksum manifest and
actual viewer screenshots are added. No private recordings or local build
output are included in the repository/source ZIP.

## Host

Built using Visual Studio Build Tools / MSVC x64 with:

```
-LcdSum3Default -LowLatencyDefault -BufferedPaintDefault
```

All seven CTest targets passed:

- lcd-field-reconstruction
- lcd-bus-packing
- logic-probe
- lcd-sync-confirmation
- frame-slot-ring
- capture-cadence
- buffered-gdi-frame

These tests verify particular algorithms and presentation behavior; they do
not prove perfect visual fidelity, physical input latency or universal
hardware compatibility. The rebuilt viewer is not substituted for the
already accepted release EXE.

## Firmware

The staged source builds the specific
`supervision_capture_fast_field_handoff` target with Pico SDK 1.5.1 and ARM GCC
10.3.1, for `PICO_BOARD=pico`. The rebuilt UF2 has the **same SHA256** as the
accepted firmware:

`EC7F5DE0546866548243D49D213384FE20589A36E503BA5ABCDE98C80A796CF6`

No flashing was performed during packaging or build verification. This
matching build is evidence for this toolchain/configuration, not a guarantee
that all future toolchains produce identical files.

## Assets and screenshots

Viewer and UF2 assets are copied from the accepted binary files and verified
against `BINARY_SHA256SUMS.txt`. Renaming them for the package does not change
their contents. Screenshots show the actual accepted viewer window in green
and neutral-gray palettes, captured at different game moments; they are not
generated or retouched. The viewer's green palette was restored afterward.
