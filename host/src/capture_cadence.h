#pragma once
#include <cstdint>

// Source cadence uses sequence distance, INCLUDING lost frames, and the Pico
// clock. It must not be confused with receive/decoded/paint throughput.
class CaptureCadence {
public:
    double push(uint32_t sequence, uint64_t timestamp_us) {
        const uint32_t distance = sequence - last_sequence_;
        if (!primed_ || timestamp_us <= last_time_ ||
            timestamp_us - last_time_ > 1000000u || distance >= 0x80000000u) {
            first_sequence_ = sequence;
            first_time_ = timestamp_us;
            fps_ = 0;
            primed_ = true;
        } else if (distance != 0 && timestamp_us - first_time_ >= 1000000u) {
            fps_ = static_cast<double>(sequence - first_sequence_) * 1000000.0 /
                   static_cast<double>(timestamp_us - first_time_);
            if (timestamp_us - first_time_ >= 10000000u) {
                first_sequence_ = sequence;
                first_time_ = timestamp_us;
            }
        }
        last_sequence_ = sequence;
        last_time_ = timestamp_us;
        return fps_;
    }
private:
    uint32_t first_sequence_ = 0, last_sequence_ = 0;
    uint64_t first_time_ = 0, last_time_ = 0;
    double fps_ = 0;
    bool primed_ = false;
};
