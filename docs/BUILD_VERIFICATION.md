# v0.1.0 build verification — 2026-09-30

This document records build checks performed on the release source. Build
success and automated tests are distinct from real-console visual testing:
neither alone establishes universal compatibility or exact LCD fidelity.

## Windows viewer

Built using Visual Studio Build Tools / MSVC x64 with:

```powershell
.\scripts\build-windows.ps1 -LcdSum3Default -LowLatencyDefault -BufferedPaintDefault
```

These options enable three-field grayscale reconstruction, low-latency USB
reads and buffered drawing. All seven CTest targets passed:

- lcd-field-reconstruction
- lcd-bus-packing
- logic-probe
- lcd-sync-confirmation
- frame-slot-ring
- capture-cadence
- buffered-gdi-frame

The tests check specific algorithms, packing, synchronization, queueing,
cadence and drawing behavior. They do not measure physical button-to-screen
latency or prove perfect visual fidelity. The prebuilt Windows EXE supplied
with the release was also tested with a real console.

## Pico firmware

Built `supervision_capture_fast_field_handoff` with Pico SDK 1.5.1, ARM GCC
10.3.1 and `PICO_BOARD=pico`:

```powershell
.\scripts\build-firmware.ps1 -Targets supervision_capture_fast_field_handoff
```

The resulting UF2 matches the distributed `SupervisionCapture.uf2` SHA256:

`EC7F5DE0546866548243D49D213384FE20589A36E503BA5ABCDE98C80A796CF6`

This match applies to that toolchain and configuration. Other toolchain
versions or build metadata may produce different binary hashes.

## Packages and screenshots

The packaged viewer and firmware are checked against `BINARY_SHA256SUMS.txt`.
`SHA256SUMS.txt` checks the two attached ZIP downloads. The source package
contains source, tests, build scripts, documentation and licenses, but no
private recordings, ROMs or local build output.

README screenshots show the viewer running in green and neutral grayscale
palettes at different game moments. They are actual application captures,
not generated images.

The post-publication documentation correction changes installation and
public-facing explanations only. Runtime source and the EXE/UF2 hashes are
unchanged.
