#pragma once
#include "logic_probe_analysis.h"

// Synthetic waveform only. Never emitted onto the console GPIOs.
enum class ProbeTestFault { none, missing_clock, extra_clock, extra_latch, data_at_clock };
inline ProbeCapture probe_test_signal(bool data = false, ProbeTestFault fault = ProbeTestFault::none) {
    ProbeCapture c;
    const uint32_t factor = data ? 2u : 6u;
    const uint32_t line_ticks = 246u * factor, field_ticks = line_ticks * 160u;
    const uint32_t bits = data ? 8u : 3u, per_word = data ? 4u : 10u;
    const uint32_t count = (SV_PROBE_MAX_PAYLOAD / 4u) * per_word;
    c.payload.resize(SV_PROBE_MAX_PAYLOAD);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t in_line = i % line_ticks;
        bool clock = in_line < 240u * factor && in_line % (6u * factor) < factor;
        bool latch = in_line >= 240u * factor && in_line < 241u * factor;
        const bool polarity = ((i / field_ticks) & 1u) == 0;
        const uint32_t missing = field_ticks + 60u * line_ticks + 5u * 6u * factor;
        const uint32_t extra = field_ticks + 50u * line_ticks + 2u * factor;
        const uint32_t latch_extra = field_ticks + 40u * line_ticks + 37u * 6u * factor + 3u * factor;
        if (fault == ProbeTestFault::missing_clock && i >= missing && i < missing + factor) clock = false;
        if (fault == ProbeTestFault::extra_clock && i == extra) clock = true;
        if (fault == ProbeTestFault::extra_latch && i == latch_extra) latch = true;
        uint32_t value = (clock ? 1u : 0u) | (polarity ? 2u : 0u) | (latch ? 4u : 0u);
        if (data) {
            const uint32_t delay = fault == ProbeTestFault::data_at_clock ? 0u : 4u;
            const uint32_t nibble = i >= delay ? ((i - delay) / 12u) & 15u : 15u;
            value = (value << 4) | nibble;
        }
        const uint32_t word_index = i / per_word;
        const uint32_t shift = 32u - bits * per_word + (i % per_word) * bits;
        const uint32_t word = sv_read_le32(c.payload.data() + word_index * 4u) | (value << shift);
        sv_write_le32(c.payload.data() + word_index * 4u, word);
    }
    std::copy_n(reinterpret_cast<const uint8_t *>("SVW0"), 4, c.header.data());
    sv_write_le16(c.header.data() + 4, 1);
    sv_write_le16(c.header.data() + 6, SV_PROBE_HEADER_SIZE);
    sv_write_le32(c.header.data() + 8, 0x54455354u); // TEST, not a hardware acquisition.
    sv_write_le32(c.header.data() + 12, data ? 8000000u : 24000000u);
    sv_write_le32(c.header.data() + 16, count);
    sv_write_le32(c.header.data() + 20, SV_PROBE_MAX_PAYLOAD);
    c.header[32] = data ? 16 : 20; c.header[33] = data ? 7 : 3;
    c.header[34] = static_cast<uint8_t>(bits); c.header[35] = static_cast<uint8_t>(per_word);
    sv_write_le32(c.header.data() + 40, 240000000u);
    uint32_t crc = 0xffffffffu;
    for (uint8_t byte : c.payload) crc = sv_probe_crc_update(crc, byte);
    sv_write_le32(c.header.data() + 44, crc ^ 0xffffffffu);
    c.validate(); return c;
}
