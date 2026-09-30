# Changelog

## v0.1.0 — 2026-09-30

First accepted release of real-console USB video capture for Windows x64.

- RP2040 capture with real LINE_LATCH synchronization, glitch qualification,
  and fast capture-to-USB buffer handoff.
- Raw chronological field pairs over WinUSB, approximately 50.815 packets/s
  on the tested console; sequence/timestamp/drop counters.
- Four grayscale levels reconstructed from three chronological fields.
- Responsive 1024-byte USB reads and latest-frame painting.
- Off-screen GDI composition with completed-only presentation; previously
  observed whole-viewer black clears fixed in user testing.
- Fullscreen, palette selection, PNG snapshots, raw `.svf` recording and timing
  logs.
- Significant AI-assisted development disclosed in README.

Known issue: movement, particularly the paddle, can remain uneven and moving
edges can show temporal mixing. No interpolation or perfect-fidelity claim.
Later DXGI and GDI-worker experiments are not promoted into this release.
