#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include "lcd_sync_confirmation.h"
#include "logic_probe_test_signal.h"

static void check(bool ok, const char *message) {
    if (!ok) { std::fprintf(stderr, "FAILED: %s\n", message); std::exit(1); }
}

struct Counts { uint64_t clocks = 0, latches = 0, polarity = 0, lines = 0, bad = 0; };

// Same endpoint predicate as firmware, replayed at a specified observation gap.
// This is not an emulation of the CPU polling latency or USB/core scheduling.
static Counts replay(const ProbeCapture &c, uint32_t gap) {
    Counts out;
    uint32_t previous = c.sample(0) << 20;
    uint8_t accepted = static_cast<uint8_t>((previous >> 21) & 1u);
    uint32_t clocks_line = 0;
    bool have_latch = false;
    for (uint32_t i = 1; i + gap < c.count; ++i) {
        const uint32_t first = c.sample(i) << 20;
        const uint32_t second = c.sample(i + gap) << 20;
        const uint32_t value = sv_lcd_sync_events(first, previous) ?
            sv_confirm_lcd_sync(first, second, accepted) : first;
        if (((value >> 21) & 1u) != accepted) {
            accepted = static_cast<uint8_t>((value >> 21) & 1u); ++out.polarity;
        }
        if ((value & (1u << 20)) && !(previous & (1u << 20))) {
            ++out.clocks; ++clocks_line;
        }
        if ((value & (1u << 22)) && !(previous & (1u << 22))) {
            ++out.latches;
            if (have_latch) { ++out.lines; if (clocks_line != 40) ++out.bad; }
            have_latch = true; clocks_line = 0;
        }
        previous = (value & ~(1u << 21)) | (static_cast<uint32_t>(accepted) << 21);
    }
    return out;
}

static ProbeCapture load(const std::filesystem::path &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot read trace");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    if (bytes.size() < SV_PROBE_HEADER_SIZE) throw std::runtime_error("Short trace");
    ProbeCapture c;
    std::copy_n(bytes.begin(), c.header.size(), c.header.begin());
    c.payload.assign(bytes.begin() + c.header.size(), bytes.end());
    c.validate();
    if (c.base != 20) throw std::runtime_error("Replay requires the 24 MHz timing format");
    return c;
}

int main(int argc, char **argv) {
    // Exhaust all seven captured input pins and both accepted polarities.
    for (uint32_t first = 0; first < 128; ++first)
        for (uint32_t second = 0; second < 128; ++second)
            for (uint8_t accepted = 0; accepted < 2; ++accepted) {
                const uint32_t v = sv_confirm_lcd_sync(first << 16, second << 16, accepted) >> 16;
                check((v & 15) == (first & 15), "DATA keeps original sampling phase");
                check((v & 80) == (first & second & 80), "CLOCK/LATCH require confirmation");
                const uint32_t expected = ((first ^ second) & 32) ? accepted * 32u : first & 32u;
                check((v & 32) == expected, "POLARITY must persist or retain accepted phase");
                const uint32_t pending = sv_lcd_sync_events(first << 16, second << 16) >> 16;
                check(pending == (((first ^ second) & first & 80) | ((first ^ second) & 32)), "only prospective rising sync or polarity changes need confirmation");
            }
    // The measured full-bus glitch 15 -> 112 -> 0 must not pick a high plane.
    const uint32_t glitch = sv_confirm_lcd_sync(112u << 16, 0, 0);
    check((glitch & (7u << 20)) == 0, "reject simultaneous CLOCK/POLARITY/LATCH spike");
    check(sv_confirm_lcd_sync(15u << 16, 0, 0) == (15u << 16), "confirmation never resamples DATA");
    const auto clean = probe_test_signal(false);
    const auto extra = probe_test_signal(false, ProbeTestFault::extra_clock);
    const auto missing = probe_test_signal(false, ProbeTestFault::missing_clock);
    for (uint32_t gap = 1; gap <= 4; ++gap) {
        const auto a = replay(clean, gap), b = replay(extra, gap), m = replay(missing, gap);
        check(a.bad == 0 && a.lines > 300, "retain real 250 ns clocks and line latches");
        check(a.clocks == b.clocks && b.bad == 0, "reject narrow extra clock");
        check(m.bad == 1 && m.clocks + 1 == a.clocks, "do not invent missing clock");
    }
    std::puts("PASS: exhaustive sync predicate, unchanged DATA, clean timing, spike rejection, missing clock retained");

    if (argc == 2) {
        uint64_t files = 0;
        Counts raw{}, confirmed{};
        for (const auto &entry : std::filesystem::directory_iterator(argv[1])) {
            const auto name = entry.path().filename().string();
            if (name.rfind("REAL-timing-20260930-02-", 0) != 0 || entry.path().extension() != ".svw") continue;
            const auto c = load(entry.path());
            const auto r = replay(c, 0);
            raw.bad += r.bad;
            for (uint32_t gap = 1; gap <= 4; ++gap) {
                const auto a = replay(c, gap);
                check(a.bad == 0, "real traces: all confirmed lines have exactly 40 clocks");
                check(a.polarity == 2, "real traces: two field transitions, no brief polarity spikes");
                if (gap == 2) {
                    confirmed.lines += a.lines; confirmed.clocks += a.clocks;
                    confirmed.latches += a.latches; confirmed.polarity += a.polarity;
                }
            }
            ++files;
        }
        check(files == 30, "exactly thirty real timing traces replayed");
        check(raw.bad == 617 && confirmed.lines == 9966, "measured baseline counts reproduced");
        std::printf("PASS REAL REPLAY: %llu captures, %llu -> 0 non-40 intervals, %llu valid lines, %llu clocks, %llu latches, %llu field edges\n",
            static_cast<unsigned long long>(files), static_cast<unsigned long long>(raw.bad),
            static_cast<unsigned long long>(confirmed.lines), static_cast<unsigned long long>(confirmed.clocks),
            static_cast<unsigned long long>(confirmed.latches), static_cast<unsigned long long>(confirmed.polarity));
    }
}
