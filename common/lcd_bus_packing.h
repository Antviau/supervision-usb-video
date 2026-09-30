#ifndef SUPERVISION_LCD_BUS_PACKING_H
#define SUPERVISION_LCD_BUS_PACKING_H

#include <stdint.h>

// The firmware supplies a RAM section attribute; host tests need none.
#ifndef SV_PACKING_RAM_ATTRIBUTE
#define SV_PACKING_RAM_ATTRIBUTE
#endif

static const uint8_t sv_low_field_lookup[16] SV_PACKING_RAM_ATTRIBUTE = {
    0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15,
    0x40, 0x41, 0x44, 0x45, 0x50, 0x51, 0x54, 0x55,
};

static inline uint8_t sv_pack_low_field(uint8_t data) {
    return sv_low_field_lookup[data]; // Caller masks DATA0..DATA3 to 0..15.
}

static inline uint8_t sv_pack_high_field(uint8_t data) {
    return (uint8_t)(sv_low_field_lookup[data] << 1);
}

#endif
