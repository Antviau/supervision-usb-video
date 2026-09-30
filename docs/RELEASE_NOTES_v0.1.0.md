# Supervision USB Video v0.1.0

Display video from a real Watara Supervision console on a Windows 10/11 x64
PC using a Raspberry Pi Pico (RP2040). The Pico captures the console's LCD
signals and sends them over USB to the viewer. A working original LCD is not
required; you play using the console's own controls. This is not an emulator,
USB webcam or HDMI adapter, and it does not capture audio.

## Download and install

1. Download `supervision-usb-video-v0.1.0-windows-x64.zip` from this release's
   assets and extract it to a writable folder.
2. With all power disconnected, wire the Pico to the console mainboard's LCD
   connector using the eight connections in the installation guide.
3. Flash the included `SupervisionCapture.uf2` onto the Pico using BOOTSEL.
4. **Install the WinUSB driver using Zadig. This step is mandatory.** Select
   `Supervision USB video capture`, verify USB ID **CAFE / 4020**, choose
   **WinUSB**, and install the driver with administrator approval. Do not
   select the `RPI-RP2` bootloader or another USB device.
5. Connect the Pico normally, open `SupervisionViewer.exe`, and power on the
   console.

Follow the [full installation guide](https://github.com/Antviau/supervision-usb-video/blob/main/README.md)
for wiring, electrical precautions, flashing, the complete Zadig procedure,
controls, troubleshooting and source builds. Zadig is downloaded separately
from [its official website](https://zadig.akeo.ie/).
Existing installations with this firmware and a working WinUSB driver do not
need reflashing or driver reinstallation.

## Features

- 160 x 160 video with four shades, green or neutral grayscale palette.
- Approximately 50.8 video updates per second on the tested console.
- Grayscale reconstructed from three successive LCD fields.
- Low-latency USB capture and buffered latest-frame display.
- F11 fullscreen, G palette switch, P native-resolution PNG screenshots.
- Raw `.svf` video recording and optional timing logs.

## Downloads and checksums

- `supervision-usb-video-v0.1.0-windows-x64.zip`: Windows viewer, Pico firmware,
  instructions and licenses.
- `supervision-usb-video-v0.1.0-source.zip`: application and firmware source,
  tests, build scripts, documentation and licenses.
- `SHA256SUMS.txt`: SHA256 checksums for the two ZIP files.

GitHub's automatic "Source code" archives are for building, not running the
viewer. The manually attached source ZIP includes the corrected installation
documentation. `BINARY_SHA256SUMS.txt` inside the packages identifies the
viewer and firmware:

Viewer SHA256:
`162C5DCB9648B1A73063B12F71A8577CCD1A66DB6EFE5CC3CBB76ED14CB3895B`

Firmware SHA256:
`EC7F5DE0546866548243D49D213384FE20589A36E503BA5ABCDE98C80A796CF6`

The documentation was corrected after publication to make Zadig mandatory
and provide self-contained instructions. Application/firmware source and
the EXE/UF2 binaries are unchanged. The original v0.1.0 Git tag retains the
initial documentation; use the installation guide linked above or the
updated attached packages for setup.

## Known limitations

Motion can be uneven, and moving edges can show temporal mixing from the
grayscale reconstruction. No motion interpolation is used. Testing covers
one modified console and Windows setup, not every hardware variant. The
RP2040 runs at 240 MHz; check signal voltages, wiring and stability on your
hardware. No claim of zero latency or exact physical-LCD equivalence is made.

## Verification

The release source builds successfully, and all seven host tests pass.
Firmware rebuilt with Pico SDK 1.5.1 matches the distributed UF2 byte-for-byte.
See [build verification](https://github.com/Antviau/supervision-usb-video/blob/main/docs/BUILD_VERIFICATION.md)
for configurations and test scope. The repository and source ZIP contain no
ROMs, private recordings, credentials or local build output.

## AI assistance and licensing

Developed in large part with AI assistance using OpenAI ChatGPT and Codex,
with human-directed hardware work and testing.

Original contributions are MIT. Capture firmware derived from DutchMaker's
Supervision-LCD-v2 retains its non-commercial license. The complete firmware
is therefore not unrestricted MIT/OSI-approved open source. Forks and
non-commercial improvements are welcome subject to the included licenses;
MIT does not override upstream restrictions. Read `THIRD_PARTY_NOTICES.md`
and the `licenses` directory before reuse.
