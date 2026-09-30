#include <cstdio>
#include <cstdlib>
#include "logic_probe_test_signal.h"

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}
static bool rejects(ProbeCapture c) {
    try { c.validate(); return false; } catch (const std::exception &) { return true; }
}
int main() {
    uint32_t crc = 0xffffffffu;
    for (char ch : std::string("123456789")) crc = sv_probe_crc_update(crc, static_cast<uint8_t>(ch));
    check((crc ^ 0xffffffffu) == 0xcbf43926u, "standard CRC32 check vector");
    for (bool data : {false, true}) {
        auto c = probe_test_signal(data);
        const auto a = analyze_probe(c);
        check(a.clocks_per_line.size() == 1 && a.clocks_per_line.count(40) == 1, "40 clocks per line");
        check(a.anomalous_lines.empty(), "no false anomalies");
        check(a.fields.size() == 1 && a.fields[0].clocks == 6400 && a.fields[0].latches == 160, "one complete field, partial fields excluded");
        check(a.clock_high_ticks.size() == 1 && a.clock_high_ticks.count(data ? 2u : 6u), "250 ns clock high");
        if (data) {
            check(a.min_setup_ticks == 8 && a.min_hold_ticks == 4, "data setup/hold margins");
            check(a.data_clock_same_sample == 0, "no simultaneous data transition");
        }
        c.payload[17] ^= 1; check(rejects(c), "reject corrupted payload");
        c = probe_test_signal(data); c.header[35] = 0; check(rejects(c), "reject zero samples-per-word before division");
        c = probe_test_signal(data); sv_write_le32(c.header.data() + 36, SV_PROBE_FLAG_RX_STALL); check(rejects(c), "reject FIFO stall");
    }
    auto a = analyze_probe(probe_test_signal(false, ProbeTestFault::missing_clock));
    check(a.clocks_per_line.count(39) && a.anomalous_lines.size() == 1 && a.fields[0].clocks == 6399, "missing clock found");
    a = analyze_probe(probe_test_signal(false, ProbeTestFault::extra_clock));
    check(a.clocks_per_line.count(41) && a.clock_high_ticks.count(1) && a.fields[0].clocks == 6401, "extra narrow clock found");
    a = analyze_probe(probe_test_signal(false, ProbeTestFault::extra_latch));
    check(a.anomalous_lines.size() == 2 && a.fields[0].latches == 161, "spurious latch splits a row");
    a = analyze_probe(probe_test_signal(true, ProbeTestFault::data_at_clock));
    check(a.data_clock_same_sample > 0 && a.min_setup_ticks == 0 && a.min_hold_ticks == 0, "data/clock collision found");
    std::puts("PASS: packing, CRC, both sampling modes, missing/extra clocks, spurious latch, data margins, invalid records");
}
