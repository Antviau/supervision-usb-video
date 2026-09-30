/*
 * Exact Supervision capture/buffering port from DutchMaker/Supervision-LCD-v2.
 *
 * Capture-critical behavior is intentionally kept equivalent to the reference:
 * - GPIO16..21 inputs with pull-downs
 * - 240 MHz core clock
 * - rising-edge polling from SRAM
 * - low/high polarity boundary synchronization
 * - three fields accumulated into byte-per-pixel framebuffers
 * - two framebuffers handed over through render_buffer_index + sync
 *
 * DutchMaker's implementation is Copyright (C) 2025 Ruud van Falier and is
 * used for this personal, non-commercial project under its NON-COMMERCIAL USE
 * LICENSE.  Packing the completed framebuffer and sending it over WinUSB are
 * additions isolated to core 1, after the reference capture handoff.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "tusb.h"

#include "protocol.h"

enum {
    PIN_DATA0 = 16,
    PIN_DATA1 = 17,
    PIN_DATA2 = 18,
    PIN_DATA3 = 19,
    PIN_PIXEL_CLOCK = 20,
    PIN_FRAME_POLARITY = 21,

    PIXELS_PER_FRAME = SV_FRAME_WIDTH * SV_FRAME_HEIGHT,
    FRAME_QUEUE_DEPTH = 4,
    WIRE_FRAME_SIZE = SV_FRAME_HEADER_SIZE + SV_FRAME_PAYLOAD_SIZE,
    REFERENCE_RATE_MILLIHZ = SV_LCD_FIELD_RATE_MILLIHZ / 3u,
};

typedef struct {
    uint8_t bytes[WIRE_FRAME_SIZE];
} frame_slot_t;

// These two byte-per-pixel buffers and the handshake mirror DutchMaker v2.
static uint8_t framebuffers[2][PIXELS_PER_FRAME] __attribute__((aligned(4)));
static volatile uint8_t render_buffer_index;
static volatile uint8_t sync;

static frame_slot_t frame_queue[FRAME_QUEUE_DEPTH];
static queue_t free_slots;
static queue_t ready_slots;

static uint8_t tx_slot;
static bool tx_active;
static uint32_t tx_offset;
static uint8_t status_packet[SV_STATUS_SIZE];
static uint32_t status_tx_offset;
static uint64_t next_status_us;

static volatile uint32_t sequence;
static volatile uint32_t dropped_frames;
static volatile uint32_t captured_groups;

static inline uint8_t *get_framebuffer(uint8_t index) {
    return framebuffers[index != 0 ? 1 : 0];
}

static void write_frame_header(uint8_t *header, uint32_t frame_sequence,
                               uint64_t timestamp_us) {
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
    sv_write_le32(header + SV_HEADER_RATE_OFFSET, REFERENCE_RATE_MILLIHZ);
    sv_write_le32(header + SV_HEADER_DROPPED_OFFSET, dropped_frames);
    sv_write_le32(header + SV_HEADER_FLAGS_OFFSET, 0);
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

static void pack_reference_frame(void) {
    if (sync == 0) return;

    uint8_t slot_index;
    const uint32_t frame_sequence = sequence++;
    if (!queue_try_remove(&free_slots, &slot_index)) {
        ++dropped_frames;
        sync = 0;
        return;
    }

    frame_slot_t *slot = &frame_queue[slot_index];
    uint8_t *destination = slot->bytes + SV_FRAME_HEADER_SIZE;
    const uint8_t *source = get_framebuffer(render_buffer_index);
    for (uint32_t group = 0; group < SV_FRAME_PAYLOAD_SIZE; ++group) {
        const uint32_t pixel = group * 4u;
        destination[group] =
            (uint8_t)((source[pixel + 0u] & 3u) << 0u) |
            (uint8_t)((source[pixel + 1u] & 3u) << 2u) |
            (uint8_t)((source[pixel + 2u] & 3u) << 4u) |
            (uint8_t)((source[pixel + 3u] & 3u) << 6u);
    }
    write_frame_header(slot->bytes, frame_sequence, time_us_64());
    queue_add_blocking(&ready_slots, &slot_index);

    // Equivalent to lcd_render_framebuffer() clearing sync after it has
    // completely consumed the selected reference framebuffer.
    sync = 0;
}

static void usb_tx_service(void) {
    if (!tud_mounted()) return;

    if (!tx_active && status_tx_offset == 0 &&
        queue_try_remove(&ready_slots, &tx_slot)) {
        tx_active = true;
    }

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
        queue_add_blocking(&free_slots, &tx_slot);
    }
}

void tud_umount_cb(void) {
    if (tx_active) {
        queue_try_add(&free_slots, &tx_slot);
        tx_active = false;
    }
    uint8_t slot_index;
    while (queue_try_remove(&ready_slots, &slot_index)) {
        queue_try_add(&free_slots, &slot_index);
    }
    tx_offset = 0;
    status_tx_offset = 0;
}

void tud_suspend_cb(bool remote_wakeup_en) {
    (void)remote_wakeup_en;
}

void tud_resume_cb(void) {}

static void usb_render_core(void) {
    tusb_init();
    while (true) {
        // This replaces DutchMaker's lcd_render_framebuffer() call.  It reads
        // only the completed render buffer and clears sync when finished.
        if (sync == 1) pack_reference_frame();

        tud_task();
        usb_tx_service();
        if (tud_vendor_available()) {
            uint8_t discard[64];
            tud_vendor_read(discard, sizeof(discard));
        }
    }
}

static void supervision_gpio_init(void) {
    // Direct port of lib/supervision.c from DutchMaker v2.
    for (uint pin = PIN_DATA0; pin <= PIN_FRAME_POLARITY; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
    }
}

static void __no_inline_not_in_flash_func(reference_capture_data)(void) {
    // Direct port of capture_data() from DutchMaker v2.  Keep this path free
    // of USB, queues, timers, bounds checks, packing, and diagnostic counters.
    uint8_t last_clock_state = 0;
    uint8_t last_polarity_state = 1;
    uint8_t field = 0;
    uint8_t *buffer = get_framebuffer(1);

    supervision_gpio_init();

    while (gpio_get(PIN_FRAME_POLARITY)) {
    }
    while (!gpio_get(PIN_FRAME_POLARITY)) {
    }

    while (true) {
        const uint32_t bus = gpio_get_all();
        const uint8_t clock = (uint8_t)((bus >> PIN_PIXEL_CLOCK) & 1u);
        const uint8_t polarity = (uint8_t)((bus >> PIN_FRAME_POLARITY) & 1u);

        if (clock && !last_clock_state) {
            if (field == 0) {
                *buffer++ = (uint8_t)((bus >> PIN_DATA0) & 1u);
                *buffer++ = (uint8_t)((bus >> PIN_DATA1) & 1u);
                *buffer++ = (uint8_t)((bus >> PIN_DATA2) & 1u);
                *buffer++ = (uint8_t)((bus >> PIN_DATA3) & 1u);
            } else {
                *buffer++ += (uint8_t)((bus >> PIN_DATA0) & 1u);
                *buffer++ += (uint8_t)((bus >> PIN_DATA1) & 1u);
                *buffer++ += (uint8_t)((bus >> PIN_DATA2) & 1u);
                *buffer++ += (uint8_t)((bus >> PIN_DATA3) & 1u);
            }
        }

        if (polarity != last_polarity_state) {
            captured_groups += SV_FRAME_PAYLOAD_SIZE;
            if (field == 2) {
                if (!sync) {
                    render_buffer_index = (uint8_t)!render_buffer_index;
                    sync = 1;
                }
                field = 0;
            } else {
                ++field;
            }
            buffer = get_framebuffer((uint8_t)!render_buffer_index);
        }

        last_clock_state = clock;
        last_polarity_state = polarity;
    }
}

int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
    sleep_ms(10);

    stdio_init_all();
    queue_init(&free_slots, sizeof(uint8_t), FRAME_QUEUE_DEPTH);
    queue_init(&ready_slots, sizeof(uint8_t), FRAME_QUEUE_DEPTH);
    for (uint8_t slot = 0; slot < FRAME_QUEUE_DEPTH; ++slot) {
        queue_add_blocking(&free_slots, &slot);
    }

    // Preserve the reference startup ordering and settling delays.
    sleep_ms(50);
    multicore_launch_core1(usb_render_core);
    sleep_ms(1000);
    reference_capture_data();
}
