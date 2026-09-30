#ifndef SUPERVISION_LCD_SYNC_CONFIRMATION_H
#define SUPERVISION_LCD_SYNC_CONFIRMATION_H

#include <stdint.h>

static inline uint32_t sv_lcd_sync_events(uint32_t observed, uint32_t previous) {
    const uint32_t changed = observed ^ previous;
    return (changed & observed & ((1u << 20) | (1u << 22))) |
           (changed & (1u << 21));
}

// GPIO16..19 DATA, GPIO20 CLOCK, GPIO21 POLARITY, GPIO22 LINE_LATCH.
// A second observation must confirm CLOCK/LATCH high and a changed POLARITY.
// Keep DATA from the original snapshot: do not move the pixel sampling phase.
static inline uint32_t sv_confirm_lcd_sync(uint32_t first, uint32_t second,
                                         uint8_t accepted_polarity) {
    const uint32_t rising_mask = (1u << 20) | (1u << 22);
    const uint32_t polarity_mask = 1u << 21;
    const uint32_t polarity = ((first ^ second) & polarity_mask) == 0 ?
        (first & polarity_mask) : ((uint32_t)accepted_polarity << 21);
    return (first & ~(rising_mask | polarity_mask)) |
           (first & second & rising_mask) | polarity;
}

#endif
