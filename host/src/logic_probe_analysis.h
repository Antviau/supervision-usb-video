#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "logic_probe_protocol.h"

struct ProbeCapture {
    std::array<uint8_t, SV_PROBE_HEADER_SIZE> header{};
    std::vector<uint8_t> payload;
    uint32_t rate = 0, count = 0, base = 0, pins = 0, bits = 0, per_word = 0;

    void validate() {
        if (std::string(reinterpret_cast<const char *>(header.data()), 4) != "SVW0" ||
            sv_read_le16(header.data() + 4) != 1 ||
            sv_read_le16(header.data() + 6) != header.size())
            throw std::runtime_error("Ce Pico ne repond pas avec le protocole de diagnostic SVW0.");
        if (sv_read_le32(header.data() + 36) != 0)
            throw std::runtime_error("Mesure invalide: timeout, FIFO ou DMA (flags=" +
                std::to_string(sv_read_le32(header.data() + 36)) + "). Console allumee ?");
        rate = sv_read_le32(header.data() + 12);
        count = sv_read_le32(header.data() + 16);
        base = header[32]; pins = header[33]; bits = header[34]; per_word = header[35];
        const bool timing = base == 20 && pins == 3 && bits == 3 && per_word == 10 && rate == 24000000;
        const bool data = base == 16 && pins == 7 && bits == 8 && per_word == 4 && rate == 8000000;
        if ((!timing && !data) || count == 0 || count > 491520 ||
            payload.size() != sv_read_le32(header.data() + 20) ||
            payload.size() > SV_PROBE_MAX_PAYLOAD ||
            payload.size() != ((count + per_word - 1u) / per_word) * 4u)
            throw std::runtime_error("Dimensions de mesure invalides.");
        uint32_t crc = 0xffffffffu;
        for (uint8_t byte : payload) crc = sv_probe_crc_update(crc, byte);
        if ((crc ^ 0xffffffffu) != sv_read_le32(header.data() + 44))
            throw std::runtime_error("CRC de mesure incorrect: donnees rejetees.");
    }
    uint32_t sample(uint32_t i) const {
        return sv_probe_sample(payload.data(), i, bits, per_word) & ((1u << pins) - 1u);
    }
    bool pin(uint32_t value, unsigned gpio) const {
        return (value & (1u << (gpio - base))) != 0;
    }
};

struct ProbeField { uint32_t start, end, clocks, latches; };
struct ProbeAnalysis {
    uint32_t clock_edges = 0, latch_edges = 0;
    std::map<uint32_t, uint32_t> clocks_per_line;
    std::map<uint32_t, uint32_t> clock_high_ticks, clock_period_ticks, latch_period_ticks;
    std::vector<ProbeField> fields;
    std::vector<std::pair<uint32_t, uint32_t>> anomalous_lines;
    uint32_t min_setup_ticks = UINT32_MAX, min_hold_ticks = UINT32_MAX;
    uint32_t data_clock_same_sample = 0;
};

inline ProbeAnalysis analyze_probe(const ProbeCapture &c) {
    ProbeAnalysis a;
    uint32_t previous = c.sample(0), clocks_line = 0, field_clocks = 0, field_latches = 0;
    uint32_t last_clock = UINT32_MAX, high_start = UINT32_MAX, last_latch = UINT32_MAX;
    uint32_t field_start = UINT32_MAX;
    std::vector<uint32_t> rising_clocks, data_changes;
    for (uint32_t i = 1; i < c.count; ++i) {
        const uint32_t value = c.sample(i);
        if (c.pin(value, 21) != c.pin(previous, 21)) {
            if (field_start != UINT32_MAX)
                a.fields.push_back({field_start, i, field_clocks, field_latches});
            field_start = i; field_clocks = field_latches = 0;
        }
        if (c.pin(value, 20) && !c.pin(previous, 20)) {
            ++a.clock_edges; ++clocks_line; ++field_clocks;
            if (last_clock != UINT32_MAX) ++a.clock_period_ticks[i - last_clock];
            last_clock = high_start = i;
            if (c.base == 16) rising_clocks.push_back(i);
        }
        if (!c.pin(value, 20) && c.pin(previous, 20) && high_start != UINT32_MAX) {
            ++a.clock_high_ticks[i - high_start]; high_start = UINT32_MAX;
        }
        if (c.pin(value, 22) && !c.pin(previous, 22)) {
            ++a.latch_edges; ++field_latches;
            if (last_latch != UINT32_MAX) {
                ++a.clocks_per_line[clocks_line];
                ++a.latch_period_ticks[i - last_latch];
                if (clocks_line != 40) a.anomalous_lines.push_back({i, clocks_line});
            }
            clocks_line = 0; last_latch = i;
        }
        if (c.base == 16 && (value & 15u) != (previous & 15u)) data_changes.push_back(i);
        previous = value;
    }
    for (uint32_t edge : rising_clocks) {
        const auto after = std::lower_bound(data_changes.begin(), data_changes.end(), edge);
        if (after != data_changes.end()) {
            a.min_hold_ticks = std::min(a.min_hold_ticks, *after - edge);
            if (*after == edge) { ++a.data_clock_same_sample; a.min_setup_ticks = 0; }
        }
        if (after != data_changes.begin()) a.min_setup_ticks = std::min(a.min_setup_ticks, edge - *(after - 1));
    }
    return a;
}

inline std::string probe_report(const ProbeCapture &c, const ProbeAnalysis &a) {
    std::ostringstream out;
    out << "SUPERvision DIGITAL LOGIC PROBE -- not video\n"
        << "sample_hz=" << c.rate << " samples=" << c.count
        << " duration_ms=" << c.count * 1000.0 / c.rate << "\n"
        << "pin_base=" << c.base << " pin_count=" << c.pins
        << " CRC_OK=1 flags=0\n"
        << "clock_rising_edges=" << a.clock_edges << " latch_rising_edges=" << a.latch_edges << "\n";
    auto histogram = [&](const char *name, const std::map<uint32_t, uint32_t> &bins, bool time) {
        out << name << ":\n";
        for (const auto &bin : bins) {
            out << "  " << bin.first;
            if (time) out << " ticks (" << bin.first * 1000000.0 / c.rate << " us)";
            out << " : " << bin.second << "\n";
        }
    };
    histogram("clocks_per_complete_latch_interval", a.clocks_per_line, false);
    histogram("clock_high_width", a.clock_high_ticks, true);
    histogram("clock_rising_period", a.clock_period_ticks, true);
    histogram("latch_rising_period", a.latch_period_ticks, true);
    out << "complete_fields=" << a.fields.size() << " (first/last partial excluded)\n";
    for (const auto &f : a.fields)
        out << "  sample=" << f.start << " duration_us=" << (f.end - f.start) * 1000000.0 / c.rate
            << " clocks=" << f.clocks << " latches=" << f.latches << "\n";
    out << "non_40_clock_intervals=" << a.anomalous_lines.size() << "\n";
    for (const auto &line : a.anomalous_lines)
        out << "  sample=" << line.first << " clocks=" << line.second << "\n";
    if (c.base == 16) {
        out << "data_changes_same_sample_as_clock=" << a.data_clock_same_sample << "\n";
        if (a.min_setup_ticks != UINT32_MAX)
            out << "minimum_observed_setup_us=" << a.min_setup_ticks * 1000000.0 / c.rate << "\n";
        if (a.min_hold_ticks != UINT32_MAX)
            out << "minimum_observed_hold_us=" << a.min_hold_ticks * 1000000.0 / c.rate << "\n";
    }
    out << "Digital levels only; not voltage/ringing measurements. Timing quantization: one sample.\n";
    return out.str();
}
