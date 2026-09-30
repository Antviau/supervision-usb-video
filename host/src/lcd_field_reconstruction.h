#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "protocol.h"

// The existing firmware transports two chronological binary LCD fields per
// packet, not two significance-weighted bits of a grayscale pixel. Preserve
// that transport and sum the last three fields, as DutchMaker v2 does. Advancing
// by two fields per packet retains the original packet cadence (~50.8 Hz).
// Do this on the receiving thread: a UI paint skip must not lose field history.
class LcdFieldReconstruction {
public:
    bool push(uint32_t sequence, uint64_t timestamp_us, uint32_t flags,
              std::array<uint8_t, SV_FRAME_PAYLOAD_SIZE> &pixels) {
        if ((flags & SV_FRAME_FLAG_HIGH_FIELD_IS_MSB) == 0) {
            primed_ = false;
            return true; // Already reconstructed firmware: leave it untouched.
        }

        const bool contiguous = primed_ && sequence == sequence_ + 1u &&
            timestamp_us > timestamp_us_ &&
            timestamp_us - timestamp_us_ < 30000u;
        for (std::size_t group = 0; group < pixels.size(); ++group) {
            const uint8_t raw = pixels[group];
            if (contiguous) {
                uint8_t reconstructed = 0;
                for (unsigned pixel = 0; pixel < 4; ++pixel) {
                    const unsigned shift = pixel * 2;
                    const uint8_t sum = static_cast<uint8_t>(
                        ((last_low_[group] >> shift) & 1u) +
                        ((raw >> (shift + 1)) & 1u) +
                        ((raw >> shift) & 1u));
                    reconstructed |= static_cast<uint8_t>(sum << shift);
                }
                pixels[group] = reconstructed;
            }
            last_low_[group] = raw & 0x55u;
        }
        sequence_ = sequence;
        timestamp_us_ = timestamp_us;
        primed_ = true;
        // After a discontinuity, wait for an actual third field rather than
        // inventing a gray value or borrowing a field from before the gap.
        return contiguous;
    }

private:
    std::array<uint8_t, SV_FRAME_PAYLOAD_SIZE> last_low_{};
    uint32_t sequence_ = 0;
    uint64_t timestamp_us_ = 0;
    bool primed_ = false;
};
