# USB and recording protocol v1

Vendor-specific bulk interface: VID `0xCAFE`, PID `0x4020`, interface 0,
IN endpoint `0x81`, OUT endpoint `0x01`; 64-byte USB endpoint packets.
See `common/protocol.h` and `firmware/src/usb_descriptors.c` for the authority.
This is not UVC video. USB packet boundaries need not equal video boundaries.

## Frame record

All multibyte integers are little-endian. A frame record consists of a
40-byte header followed by a 6,400-byte payload.

| Offset | Bytes | Value |
| --- | ---: | --- |
| 0 | 4 | ASCII `SVF0` |
| 4 | 2 | Protocol version, 1 |
| 6 | 2 | Header length, 40 |
| 8 | 4 | Frame-pair sequence |
| 12 | 8 | Pico timestamp in microseconds |
| 20 | 2 | Width, 160 |
| 22 | 2 | Height, 160 |
| 24 | 4 | Payload size, 6400 |
| 28 | 4 | Nominal pair rate in millihertz (not measured monitor FPS) |
| 32 | 4 | Cumulative firmware drop count |
| 36 | 4 | Flags |

Flag bit 0 is named `SV_FRAME_FLAG_HIGH_FIELD_IS_MSB` for historical reasons.
In this release it indicates two raw chronological binary fields, **not**
significance-weighted bits of a grayscale sample.

Each payload byte packs four adjacent pixels in row-major order. Pixel lane
`n` occupies bits `2*n` and `2*n+1`: bit `2*n+1` is the older high-polarity
field, bit `2*n` the newer low-polarity field. The host sums previous packet
low + current high + current low to obtain each shade 0..3. Following a
sequence/timestamp discontinuity it primes history rather than mixing across
the gap. This runs before latest-frame publication to the UI.

`.svf` recording stores the received raw headers/payloads before processing.
Firmware also emits separate `SVS0` status records; they are not video frames
and are not written as frame records into `.svf`.

## Timing

Observed pair cadence is around 50.815/s, with roughly twice as many LCD
fields. Individual field timestamps are not transported. Sequence gaps and
firmware drop increments are different indicators; inspect both. Host and
Pico clocks are not synchronized. Viewer `paint` timing events indicate
application submission, not monitor scanout or button-to-photon latency.
