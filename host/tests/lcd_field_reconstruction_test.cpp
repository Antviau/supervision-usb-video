#include <cstdio>
#include <cstdlib>
#include "lcd_field_reconstruction.h"

static void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char **argv) {
    // Every possible three-field duty cycle, every starting phase, every
    // data lane. True grays must not oscillate with polarity/packet phase.
    for (unsigned cycle = 0; cycle < 8; ++cycle) {
        for (unsigned phase = 0; phase < 3; ++phase) {
            LcdFieldReconstruction decoder;
            const unsigned sum = (cycle & 1u) + ((cycle >> 1) & 1u) +
                                 ((cycle >> 2) & 1u);
            for (unsigned packet = 0; packet < 30; ++packet) {
                std::array<uint8_t, SV_FRAME_PAYLOAD_SIZE> pixels{};
                for (unsigned lane = 0; lane < 4; ++lane) {
                    const unsigned field = phase + lane + packet * 2;
                    const unsigned high = (cycle >> (field % 3)) & 1u;
                    const unsigned low = (cycle >> ((field + 1) % 3)) & 1u;
                    pixels[0] |= static_cast<uint8_t>((high * 2 + low) << (lane * 2));
                }
                pixels.fill(pixels[0]);
                const bool ready = decoder.push(packet, 1000 + packet * 19679,
                    SV_FRAME_FLAG_HIGH_FIELD_IS_MSB, pixels);
                check(ready == (packet != 0), "first packet primes history");
                if (ready) {
                    for (uint8_t packed : pixels) {
                        for (unsigned lane = 0; lane < 4; ++lane) {
                            check(((packed >> (lane * 2)) & 3u) == sum,
                                  "exact grayscale, all phases and lanes");
                        }
                    }
                }
            }
        }
    }

    LcdFieldReconstruction decoder;
    std::array<uint8_t, SV_FRAME_PAYLOAD_SIZE> pixels{};
    auto push = [&](uint32_t seq, uint64_t us, uint8_t raw, uint32_t flags =
                    SV_FRAME_FLAG_HIGH_FIELD_IS_MSB) {
        pixels.fill(raw);
        return decoder.push(seq, us, flags, pixels);
    };
    check(!push(10, 1000, 0xff), "initial history empty");
    check(push(11, 20679, 0), "consecutive frame ready");
    check(pixels[0] == 0x55, "one older field plus two new fields");
    check(!push(13, 60037, 0xff), "sequence gap must reset history");
    check(push(14, 79716, 0), "recover immediately on next packet");
    check(pixels[0] == 0x55, "recovered history contains the last real field");
    check(!push(15, 79716, 0xff), "same timestamp resets history");
    check(!push(16, 500000, 0xff), "large time discontinuity resets history");
    check(push(17, 519679, 0x96, 0), "predecoded payload passes through");
    check(pixels[0] == 0x96, "predecoded payload unchanged");
    check(!push(18, 539358, 0xff), "switching format resets history");
    check(!push(UINT32_MAX, 600000, 0xff), "nonconsecutive sequence resets");
    check(push(0, 619679, 0xff), "sequence wraparound remains consecutive");
    check(pixels[0] == 0xff, "all-black remains black");
    std::puts("PASS: all duty cycles, phases, lanes, gaps, formats, timestamps and wraparound");

    if (argc == 2) {
        FILE *file = nullptr;
        check(fopen_s(&file, argv[1], "rb") == 0 && file != nullptr,
              "open raw recording");
        LcdFieldReconstruction replay;
        unsigned packets = 0, ready_packets = 0;
        std::array<uint8_t, SV_FRAME_HEADER_SIZE> header{};
        while (true) {
            const size_t bytes = std::fread(header.data(), 1, header.size(), file);
            if (bytes == 0) break;
            check(bytes == header.size(), "complete recording header");
            check(header[0] == 'S' && header[1] == 'V' &&
                  header[2] == 'F' && header[3] == '0', "SVF signature");
            check(sv_read_le32(header.data() + SV_HEADER_PAYLOAD_OFFSET) ==
                  pixels.size(), "expected recorded payload size");
            check(std::fread(pixels.data(), 1, pixels.size(), file) == pixels.size(),
                  "complete recording payload");
            constexpr unsigned probe = 53 * SV_FRAME_WIDTH + 53;
            const unsigned raw_probe = (pixels[probe / 4] >> ((probe % 4) * 2)) & 3u;
            const bool ready = replay.push(
                sv_read_le32(header.data() + SV_HEADER_SEQUENCE_OFFSET),
                sv_read_le64(header.data() + SV_HEADER_TIMESTAMP_OFFSET),
                sv_read_le32(header.data() + SV_HEADER_FLAGS_OFFSET), pixels);
            if (ready) ++ready_packets;
            if (packets < 9) {
                const unsigned processed_probe =
                    (pixels[probe / 4] >> ((probe % 4) * 2)) & 3u;
                std::printf("packet %u: raw pixel %u, sum3 %u, ready %u\n",
                            packets, raw_probe, processed_probe, ready ? 1u : 0u);
            }
            ++packets;
        }
        check(!std::ferror(file), "recording read without error");
        std::fclose(file);
        std::printf("REPLAY PASS: %u raw packets, %u reconstructed packets, %u history resets\n",
                    packets, ready_packets, packets - ready_packets);
    }
}
