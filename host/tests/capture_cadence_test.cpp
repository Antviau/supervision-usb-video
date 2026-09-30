#include "capture_cadence.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void check(bool ok) { if (!ok) std::abort(); }
int main() {
    CaptureCadence cadence;
    double rate = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        // Deliberately omit 10% of packets. Native cadence must remain 50 Hz,
        // not the 45 Hz receive rate. Exercise uint32 sequence wrap as well.
        if (i % 10 == 5) continue;
        rate = cadence.push(0xffffff00u + i, 1234567ull + i * 20000ull);
    }
    check(std::abs(rate - 50.0) < 0.000001);
    check(cadence.push(0, 10) == 0); // Pico restart
    check(cadence.push(1, 20010) == 0);
    for (uint32_t i = 2; i <= 60; ++i) rate = cadence.push(i, 10 + i * 20000ull);
    check(std::abs(rate - 50.0) < 0.000001);
    check(cadence.push(61, 3000000) == 0); // Long pause, do not extrapolate
    check(cadence.push(60, 3020000) == 0); // Backward sequence
    std::puts("Source cadence, missing packets, rollover, restart and pause: OK");
}
