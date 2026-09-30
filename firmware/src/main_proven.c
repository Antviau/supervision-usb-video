/*
 * Six-wire Supervision capture path based on the field-capture method used by
 * DutchMaker/Supervision-LCD-v2 and xrip/watara-supervision-lcd.
 *
 * DutchMaker's implementation is Copyright (C) 2025 Ruud van Falier and is
 * used here for this personal, non-commercial project under its
 * NON-COMMERCIAL USE LICENSE.  The USB transport and packed frame queue are
 * project-specific additions.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "tusb.h"

#include "protocol.h"

#ifndef SV_POLLING_FAST
#define SV_POLLING_FAST 0
#endif
#ifndef SV_SYNC_CONFIRM
#define SV_SYNC_CONFIRM 0
#endif
#if SV_SYNC_CONFIRM
#include "lcd_sync_confirmation.h"
#endif
#ifndef SV_FAST_FIELD_HANDOFF
#define SV_FAST_FIELD_HANDOFF 0
#endif
#if SV_FAST_FIELD_HANDOFF
#include "hardware/timer.h"
#include "frame_slot_ring.h"
#endif
#if SV_POLLING_FAST
#define SV_PACKING_RAM_ATTRIBUTE __attribute__((section(".time_critical.sv_pack_lookup")))
#include "lcd_bus_packing.h"
#endif

enum {
    PIN_DATA0 = 16,
    PIN_DATA1 = 17,
    PIN_DATA2 = 18,
    PIN_DATA3 = 19,
    PIN_PIXEL_CLOCK = 20,
    PIN_FRAME_POLARITY = 21,
    PIN_LINE_LATCH = 22,

    GROUPS_PER_FIELD = SV_FRAME_PAYLOAD_SIZE,
    GROUPS_PER_LINE = SV_FRAME_WIDTH / 4,
    LINES_PER_FIELD = SV_FRAME_HEIGHT,
    FRAME_QUEUE_DEPTH = 8,
    WIRE_FRAME_SIZE = SV_FRAME_HEADER_SIZE + SV_FRAME_PAYLOAD_SIZE,
    MIN_FIELD_TIME_US = 9000,
};

typedef struct {
    uint8_t bytes[WIRE_FRAME_SIZE];
} frame_slot_t;

static frame_slot_t frame_queue[FRAME_QUEUE_DEPTH];
#if SV_FAST_FIELD_HANDOFF
static sv_slot_ring_t free_slots;
static sv_slot_ring_t ready_slots;
typedef struct {
    uint64_t timestamp_us;
    uint32_t frame_sequence;
    uint32_t dropped;
    bool valid;
} frame_metadata_t;
static frame_metadata_t frame_metadata[FRAME_QUEUE_DEPTH];

// Inline the timer read: do not call a flash veneer at the field boundary.
static inline uint64_t capture_timestamp_us(void) {
    uint32_t high, low, checked;
    do {
        high = timer_hw->timerawh;
        low = timer_hw->timerawl;
        checked = timer_hw->timerawh;
    } while (high != checked);
    return ((uint64_t)high << 32) | low;
}
#else
static queue_t free_slots;
static queue_t ready_slots;
#endif

static uint8_t tx_slot;
static bool tx_active;
static uint32_t tx_offset;
static uint8_t status_packet[SV_STATUS_SIZE];
static uint32_t status_tx_offset;
static uint64_t next_status_us;

static volatile uint32_t sequence;
static volatile uint32_t dropped_frames;
static volatile uint32_t captured_groups;

static uint8_t capture_slot;
static bool capture_slot_valid;
static bool high_field_valid;
#if !SV_POLLING_FAST
static uint32_t groups_in_field;
static bool field_overflow;
#endif
static uint32_t row_in_field;
static uint32_t groups_in_line;

static void write_frame_header(uint8_t *header, uint32_t frame_sequence,
                               uint64_t timestamp_us
#if SV_FAST_FIELD_HANDOFF
                               , uint32_t frame_dropped
#endif
                               ) {
    header[0] = 'S';
    header[1] = 'V';
    header[2] = 'F';
    header[3] = '0';
    sv_write_le16(header + SV_HEADER_VERSION_OFFSET, SV_PROTOCOL_VERSION);
    sv_write_le16(header + SV_HEADER_SIZE_OFFSET, SV_FRAME_HEADER_SIZE);
    sv_write_le32(header + SV_HEADER_SEQUENCE_OFFSET, frame_sequence);
    sv_write_le64(header + SV_HEADER_TIMESTAMP_OFFSET, timestamp_us);
    sv_write_le16(header + SV_HEADER_WIDTH_OFFSET, SV_FRAME_WIDTH);
    sv_write_le16(header + SV_HEADER_HEIGHT_OFFSET, SV_FRAME_HEIGHT);
    sv_write_le32(header + SV_HEADER_PAYLOAD_OFFSET, SV_FRAME_PAYLOAD_SIZE);
    sv_write_le32(header + SV_HEADER_RATE_OFFSET, SV_SOURCE_RATE_MILLIHZ);
#if SV_FAST_FIELD_HANDOFF
    sv_write_le32(header + SV_HEADER_DROPPED_OFFSET, frame_dropped);
#else
    sv_write_le32(header + SV_HEADER_DROPPED_OFFSET, dropped_frames);
#endif
    sv_write_le32(header + SV_HEADER_FLAGS_OFFSET,
                  SV_FRAME_FLAG_HIGH_FIELD_IS_MSB);
}

static void prepare_status_packet(void) {
    status_packet[0] = 'S';
    status_packet[1] = 'V';
    status_packet[2] = 'S';
    status_packet[3] = '0';
    sv_write_le16(status_packet + SV_STATUS_VERSION_OFFSET,
                  SV_PROTOCOL_VERSION);
    sv_write_le16(status_packet + SV_STATUS_SIZE_OFFSET, SV_STATUS_SIZE);
    sv_write_le64(status_packet + SV_STATUS_TIMESTAMP_OFFSET, time_us_64());
    sv_write_le32(status_packet + SV_STATUS_CAPTURED_WORDS_OFFSET,
                  captured_groups);
    sv_write_le32(status_packet + SV_STATUS_SEQUENCE_OFFSET, sequence);
    sv_write_le32(status_packet + SV_STATUS_DROPPED_OFFSET, dropped_frames);

    uint32_t levels = 0;
    for (uint pin = PIN_DATA0; pin <= PIN_FRAME_POLARITY; ++pin) {
        if (gpio_get(pin)) levels |= 1u << (pin - PIN_DATA0);
    }
    sv_write_le32(status_packet + SV_STATUS_GPIO_OFFSET, levels);
}

static void release_capture_slot(void) {
    if (capture_slot_valid) {
#if SV_FAST_FIELD_HANDOFF
        // Only USB may produce into the free ring. Publish invalid captures
        // through ready as well; USB reclaims them without sending a packet.
        frame_metadata[capture_slot].valid = false;
        if (!sv_slot_ring_push(&ready_slots, capture_slot)) ++dropped_frames;
#else
        queue_add_blocking(&free_slots, &capture_slot);
#endif
        capture_slot_valid = false;
    }
}

static void start_high_field(void) {
    high_field_valid = false;
#if SV_FAST_FIELD_HANDOFF
    capture_slot_valid = sv_slot_ring_pop(&free_slots, &capture_slot);
#else
    capture_slot_valid = queue_try_remove(&free_slots, &capture_slot);
#endif
    // Do not clear the payload here.  The high field overwrites every byte.
    // A 6400-byte memset at the field boundary makes polling miss the first
    // pixel clocks and permanently displaces the two bitplanes.
}

static inline void finish_line(uint8_t polarity) {
    // Pin 7 pulses after the 40 pixel clocks of every scanline.  Using it as
    // the address boundary prevents one missed/noisy PIXEL_CLOCK edge from
    // shifting every following row diagonally.  Short pulses seen before most
    // of a row has arrived are rejected as noise.
    if (groups_in_line < 32u || row_in_field >= LINES_PER_FIELD) return;

    if (capture_slot_valid && polarity != 0) {
#if SV_POLLING_FAST
        // A maximum of eight stores, kept in this RAM-resident hot path.
        // Volatile prevents GCC turning the loop into __wrap_memset in XIP
        // flash. This preserves the existing zero-padding semantics exactly.
        volatile uint8_t *payload =
            frame_queue[capture_slot].bytes + SV_FRAME_HEADER_SIZE;
#else
        uint8_t *payload =
            frame_queue[capture_slot].bytes + SV_FRAME_HEADER_SIZE;
#endif
        while (groups_in_line < GROUPS_PER_LINE) {
            payload[row_in_field * GROUPS_PER_LINE + groups_in_line] = 0;
            ++groups_in_line;
        }
    }

    ++row_in_field;
    groups_in_line = 0;
}

static void complete_pair(bool low_field_valid) {
    const uint32_t frame_sequence = sequence++;
    if (capture_slot_valid && high_field_valid && low_field_valid) {
#if SV_FAST_FIELD_HANDOFF
        frame_metadata_t *metadata = &frame_metadata[capture_slot];
        metadata->timestamp_us = capture_timestamp_us();
        metadata->frame_sequence = frame_sequence;
        metadata->dropped = dropped_frames;
        metadata->valid = true;
        if (!sv_slot_ring_push(&ready_slots, capture_slot)) ++dropped_frames;
#else
        frame_slot_t *slot = &frame_queue[capture_slot];
        write_frame_header(slot->bytes, frame_sequence, time_us_64());
        queue_add_blocking(&ready_slots, &capture_slot);
#endif
        capture_slot_valid = false;
    } else {
        ++dropped_frames;
        release_capture_slot();
    }
}

static inline uint8_t pack_high_field(uint8_t data) {
#if SV_POLLING_FAST
    return sv_pack_high_field(data);
#else
    uint8_t packed = 0;
    for (uint pixel = 0; pixel < 4; ++pixel) {
        packed |= (uint8_t)(((data >> pixel) & 1u) << (pixel * 2u + 1u));
    }
    return packed;
#endif
}

static inline uint8_t pack_low_field(uint8_t data) {
#if SV_POLLING_FAST
    return sv_pack_low_field(data);
#else
    uint8_t packed = 0;
    for (uint pixel = 0; pixel < 4; ++pixel) {
        packed |= (uint8_t)(((data >> pixel) & 1u) << (pixel * 2u));
    }
    return packed;
#endif
}

static inline void capture_group(uint32_t bus, uint8_t polarity) {
    ++captured_groups;
#if !SV_POLLING_FAST
    ++groups_in_field;
#endif

    if (row_in_field >= LINES_PER_FIELD ||
        groups_in_line >= GROUPS_PER_LINE) {
#if !SV_POLLING_FAST
        field_overflow = true;
#endif
        return;
    }

    if (capture_slot_valid) {
        uint8_t *payload = frame_queue[capture_slot].bytes + SV_FRAME_HEADER_SIZE;
        const uint8_t data = (uint8_t)((bus >> PIN_DATA0) & 0x0fu);
        const uint32_t index =
            row_in_field * GROUPS_PER_LINE + groups_in_line;
        if (polarity != 0) {
            payload[index] = pack_high_field(data);
        } else if (high_field_valid) {
            payload[index] |= pack_low_field(data);
        }
    }
    ++groups_in_line;
}

static void field_transition(uint8_t old_polarity, uint8_t new_polarity) {
    // Deliberately do not require an exact sample count here.  The proven
    // DutchMaker/xrip loops simply reset the framebuffer pointer whenever
    // polarity changes.  Requiring exactly 6400 samples rejected every field
    // on the real console because the polling loop can observe the boundary
    // one iteration before or after the last pixel-clock edge.
    // Accept the final row even when the polarity edge is observed before the
    // trailing edge of LINE_LATCH.  Every write remains bounded regardless of
    // malformed clocks, so a bad field can never corrupt RAM or freeze USB.
    finish_line(old_polarity);
    const bool field_geometry_valid = row_in_field >= 150u;

    if (old_polarity != 0) {
        high_field_valid = capture_slot_valid && field_geometry_valid;
    } else if (new_polarity != 0) {
        complete_pair(field_geometry_valid);
        start_high_field();
    }

#if !SV_POLLING_FAST
    groups_in_field = 0;
    field_overflow = false;
#endif
    row_in_field = 0;
    groups_in_line = 0;
}

static void usb_tx_service(void) {
    if (!tud_mounted()) return;

#if SV_FAST_FIELD_HANDOFF
    if (!tx_active && status_tx_offset == 0) {
        while (sv_slot_ring_pop(&ready_slots, &tx_slot)) {
            const frame_metadata_t *metadata = &frame_metadata[tx_slot];
            if (!metadata->valid) {
                (void)sv_slot_ring_push(&free_slots, tx_slot);
                continue;
            }
            write_frame_header(frame_queue[tx_slot].bytes, metadata->frame_sequence,
                               metadata->timestamp_us, metadata->dropped);
            tx_active = true;
            break;
        }
    }
#else
    if (!tx_active && status_tx_offset == 0 &&
        queue_try_remove(&ready_slots, &tx_slot)) {
        tx_active = true;
    }
#endif

    const uint64_t now = time_us_64();
    if (!tx_active && status_tx_offset == 0 && now >= next_status_us) {
        prepare_status_packet();
        status_tx_offset = 1;
        next_status_us = now + 250000u;
    }

    if (!tx_active && status_tx_offset == 0) return;

    const uint32_t available = tud_vendor_write_available();
    if (available == 0) {
        tud_vendor_flush();
        return;
    }

    if (status_tx_offset != 0) {
        const uint32_t offset = status_tx_offset - 1u;
        const uint32_t remaining = SV_STATUS_SIZE - offset;
        const uint32_t amount = remaining < available ? remaining : available;
        status_tx_offset += tud_vendor_write(status_packet + offset, amount);
        tud_vendor_flush();
        if (status_tx_offset - 1u == SV_STATUS_SIZE) status_tx_offset = 0;
        return;
    }

    const frame_slot_t *slot = &frame_queue[tx_slot];
    const uint32_t remaining = WIRE_FRAME_SIZE - tx_offset;
    const uint32_t amount = remaining < available ? remaining : available;
    tx_offset += tud_vendor_write(slot->bytes + tx_offset, amount);
    tud_vendor_flush();

    if (tx_offset == WIRE_FRAME_SIZE) {
        tx_offset = 0;
        tx_active = false;
#if SV_FAST_FIELD_HANDOFF
        (void)sv_slot_ring_push(&free_slots, tx_slot);
#else
        queue_add_blocking(&free_slots, &tx_slot);
#endif
    }
}

void tud_umount_cb(void) {
    if (tx_active) {
#if SV_FAST_FIELD_HANDOFF
        (void)sv_slot_ring_push(&free_slots, tx_slot);
#else
        queue_try_add(&free_slots, &tx_slot);
#endif
        tx_active = false;
    }
    uint8_t slot_index;
#if SV_FAST_FIELD_HANDOFF
    while (sv_slot_ring_pop(&ready_slots, &slot_index)) {
        (void)sv_slot_ring_push(&free_slots, slot_index);
    }
#else
    while (queue_try_remove(&ready_slots, &slot_index)) {
        queue_try_add(&free_slots, &slot_index);
    }
#endif
    tx_offset = 0;
    status_tx_offset = 0;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
}

void tud_resume_cb(void) {}

static void usb_core(void) {
    tusb_init();
    while (true) {
        tud_task();
        usb_tx_service();
        if (tud_vendor_available()) {
            uint8_t discard[64];
            tud_vendor_read(discard, sizeof(discard));
        }
    }
}

static void supervision_gpio_init(void) {
    // The six reference inputs plus the console's real per-line latch.
    for (uint pin = PIN_DATA0; pin <= PIN_LINE_LATCH; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
    }
}

static void __no_inline_not_in_flash_func(capture_loop)(void) {
    // Match DutchMaker v2 exactly: discard the current partial field, wait
    // through a complete low-polarity field, and begin on the following rising
    // polarity boundary.  Starting immediately in an already-active high
    // field makes the 9 ms debounce lock every subsequent pointer reset to the
    // same late offset, so the two bitplanes appear horizontally displaced.
#if SV_SYNC_CONFIRM
    // Do not lock the initial field phase onto a measured sub-100 ns spike.
    do {
        while (gpio_get(PIN_FRAME_POLARITY)) tight_loop_contents();
        busy_wait_at_least_cycles(12u);
    } while (gpio_get(PIN_FRAME_POLARITY));
    do {
        while (!gpio_get(PIN_FRAME_POLARITY)) tight_loop_contents();
        busy_wait_at_least_cycles(12u);
    } while (!gpio_get(PIN_FRAME_POLARITY));
#else
    while (gpio_get(PIN_FRAME_POLARITY)) tight_loop_contents();
    while (!gpio_get(PIN_FRAME_POLARITY)) tight_loop_contents();
#endif

#if SV_SYNC_CONFIRM
    uint32_t last_bus = (gpio_get_all() & (1u << PIN_LINE_LATCH)) |
                        (1u << PIN_FRAME_POLARITY);
#else
    uint8_t last_clock = 0;
    uint8_t last_line_latch = (uint8_t)gpio_get(PIN_LINE_LATCH);
#endif
    uint8_t last_polarity = 1;
    uint32_t last_transition_us = time_us_32();
    start_high_field();

    while (true) {
        uint32_t bus = gpio_get_all();
#if SV_SYNC_CONFIRM
        // Real 24 MHz traces contain one-sample CLOCK/LATCH/POLARITY spikes.
        // Confirm prospective events after a short RAM-only delay. Do not
        // change the packing, queue, line thresholds, or DATA sampling phase.
        const uint32_t events = sv_lcd_sync_events(bus, last_bus);
        // Avoid re-decoding every pin and spilling state on an empty poll.
        if (events == 0) {
            last_bus = bus;
            continue;
        }
        busy_wait_at_least_cycles(12u);
        bus = sv_confirm_lcd_sync(bus, gpio_get_all(), last_polarity);
#endif
        const uint8_t clock = (uint8_t)((bus >> PIN_PIXEL_CLOCK) & 1u);
        const uint8_t polarity = (uint8_t)((bus >> PIN_FRAME_POLARITY) & 1u);
        const uint8_t line_latch =
            (uint8_t)((bus >> PIN_LINE_LATCH) & 1u);

        // As in xrip's working loop, do not read the timer on every poll.  It
        // is needed only after polarity differs, roughly twice per frame.
        if (polarity != last_polarity) {
            const uint32_t now = time_us_32();
            if ((uint32_t)(now - last_transition_us) > MIN_FIELD_TIME_US) {
                field_transition(last_polarity, polarity);
                last_polarity = polarity;
                last_transition_us = now;
            }
        }

        // Reset the field pointer before accepting a pixel edge if both are
        // first observed in the same polling iteration.
#if SV_SYNC_CONFIRM
        if (clock != 0 && (last_bus & (1u << PIN_PIXEL_CLOCK)) == 0) {
#else
        if (clock != 0 && last_clock == 0) {
#endif
#if SV_SYNC_CONFIRM
            // Never choose a bitplane using an unaccepted POLARITY excursion.
            capture_group(bus, last_polarity);
#else
            capture_group(bus, polarity);
#endif
        }

#if SV_SYNC_CONFIRM
        if (line_latch != 0 && (last_bus & (1u << PIN_LINE_LATCH)) == 0) {
#else
        if (line_latch != 0 && last_line_latch == 0) {
#endif
#if SV_SYNC_CONFIRM
            finish_line(last_polarity);
#else
            finish_line(polarity);
#endif
        }

#if SV_SYNC_CONFIRM
        last_bus = (bus & ~(1u << PIN_FRAME_POLARITY)) |
                   ((uint32_t)last_polarity << PIN_FRAME_POLARITY);
#else
        last_clock = clock;
        last_line_latch = line_latch;
#endif
        tight_loop_contents();
    }
}

int main(void) {
    // DutchMaker v2 captures with a 240 MHz polling core.  USB runs alone on
    // core 1 so its service latency cannot make the polling loop miss an edge.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
    sleep_ms(10);

    stdio_init_all();
#if SV_FAST_FIELD_HANDOFF
    sv_slot_ring_init(&free_slots);
    sv_slot_ring_init(&ready_slots);
    for (uint8_t slot = 0; slot < FRAME_QUEUE_DEPTH; ++slot) {
        (void)sv_slot_ring_push(&free_slots, slot);
    }
#else
    queue_init(&free_slots, sizeof(uint8_t), FRAME_QUEUE_DEPTH);
    queue_init(&ready_slots, sizeof(uint8_t), FRAME_QUEUE_DEPTH);
    for (uint8_t slot = 0; slot < FRAME_QUEUE_DEPTH; ++slot) {
        queue_add_blocking(&free_slots, &slot);
    }
#endif

    supervision_gpio_init();
    multicore_launch_core1(usb_core);
    capture_loop();
}
