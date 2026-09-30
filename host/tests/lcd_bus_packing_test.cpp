#include <cstdio>
#include <cstdlib>
#include "lcd_bus_packing.h"

static uint8_t reference(unsigned data, unsigned plane) {
    uint8_t packed = 0;
    for (unsigned pixel = 0; pixel < 4; ++pixel)
        packed |= static_cast<uint8_t>(((data >> pixel) & 1u) << (pixel * 2 + plane));
    return packed;
}

int main() {
    for (unsigned high = 0; high < 16; ++high) {
        for (unsigned low = 0; low < 16; ++low) {
            const auto packed = static_cast<uint8_t>(
                sv_pack_high_field(static_cast<uint8_t>(high)) |
                sv_pack_low_field(static_cast<uint8_t>(low)));
            if (packed != (reference(high, 1) | reference(low, 0))) {
                std::fprintf(stderr, "FAILED: packing high %u low %u\n", high, low);
                return 1;
            }
        }
    }
    std::puts("PASS: all 256 high/low combinations preserve every data lane");
}
