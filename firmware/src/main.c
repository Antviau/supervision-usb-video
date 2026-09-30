#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "tusb.h"

#include "protocol.h"
#include "supervision_capture.pio.h"

#ifndef SV_USE_LINE_LATCH
#define SV_USE_LINE_LATCH 1
#endif

enum {
    PIN_DATA0 = 16,
    PIN_DATA1 = 17,
    PIN_DATA2 = 18,
    PIN_DATA3 = 19,
    PIN_PIXEL_CLOCK = 20,
    PIN_FRAME_POLARITY = 21,
    PIN_LINE_LATCH = 22,

    RAW_RING_BYTES = 32768,
    RAW_RING_WORDS = RAW_RING_BYTES / sizeof(uint32_t),
    RAW_RING_MASK = RAW_RING_WORDS - 1,
    SAMPLES_PER_DMA_WORD = 5,
    FIELD_SAMPLES = SV_FRAME_PAYLOAD_SIZE,
    FRAME_QUEUE_DEPTH = 8,
    WIRE_FRAME_SIZE = SV_FRAME_HEADER_SIZE + SV_FRAME_PAYLOAD_SIZE,
};

_Static_assert((RAW_RING_BYTES & (RAW_RING_BYTES - 1)) == 0,
               "DMA ring size must be a power of two");
_Static_assert(SV_FRAME_PAYLOAD_SIZE == 6400, "unexpected frame size");
_Static_assert(SV_FIELDS_PER_FRAME == 2, "capture expects two LCD fields");

static uint32_t raw_ring[RAW_RING_WORDS] __attribute__((aligned(RAW_RING_BYTES)));
static uint8_t field_samples[SV_FIELDS_PER_FRAME][FIELD_SAMPLES];

typedef struct {
    uint8_t bytes[WIRE_FRAME_SIZE];
} frame_slot_t;

static frame_slot_t frame_queue[FRAME_QUEUE_DEPTH];
static queue_t free_slots;
static queue_t ready_slots;
static uint8_t tx_slot;
static bool tx_active;
static uint32_t tx_offset;
static uint8_t status_packet[SV_STATUS_SIZE];
static uint32_t status_tx_offset;
static uint64_t next_status_us;

static int dma_channel;
static uint32_t consumer_word;
static volatile uint32_t sequence;
static volatile uint32_t dropped_frames;
static int8_t active_polarity = -1;
static bool have_high_field;
static uint32_t samples_in_field;
static bool field_overflow;

static uint32_t captured_words(void) {
    return UINT32_MAX - dma_hw->ch[dma_channel].transfer_count;
}

static inline uint8_t sample_six_bits(uint32_t word, uint32_t sample_index) {
    // With right shifting and a 30-bit autopush, five six-bit samples occupy
    // bits 2..31 in chronological order. Bits 0..1 are padding.
    return (uint8_t)((word >> (2u + sample_index * 6u)) & 0x3fu);
}

static uint32_t available_words(void) {
    const uintptr_t base = (uintptr_t)raw_ring;
    const uintptr_t write_address = dma_hw->ch[dma_channel].write_addr;
    const uint32_t producer = (uint32_t)(((write_address - base) >> 2u) & RAW_RING_MASK);
    return (producer - consumer_word) & RAW_RING_MASK;
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
    sv_write_le32(header + SV_HEADER_RATE_OFFSET, SV_SOURCE_RATE_MILLIHZ);
    sv_write_le32(header + SV_HEADER_DROPPED_OFFSET, dropped_frames);
    sv_write_le32(header + SV_HEADER_FLAGS_OFFSET,
                  SV_FRAME_FLAG_HIGH_FIELD_IS_MSB);
}

static void reconstruct_frame(frame_slot_t *slot) {
    uint8_t *payload = slot->bytes + SV_FRAME_HEADER_SIZE;

    // A complete native frame is an ordered pair of one-bit LCD fields. The
    // high-polarity field supplies bit 1 and the following low-polarity field
    // supplies bit 0. One byte contains four adjacent 2-bpp pixels.
    for (uint32_t group = 0; group < SV_FRAME_PAYLOAD_SIZE; ++group) {
        const uint8_t high = field_samples[0][group];
        const uint8_t low = field_samples[1][group];
        uint8_t packed = 0;
        for (uint32_t pixel = 0; pixel < 4; ++pixel) {
            const uint8_t value = (uint8_t)((((high >> pixel) & 1u) << 1u) |
                                            ((low >> pixel) & 1u));
            packed |= (uint8_t)(value << (pixel * 2u));
        }
        payload[group] = packed;
    }
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
                  captured_words());
    sv_write_le32(status_packet + SV_STATUS_SEQUENCE_OFFSET, sequence);
    sv_write_le32(status_packet + SV_STATUS_DROPPED_OFFSET, dropped_frames);

    uint32_t levels = 0;
    for (uint pin = PIN_DATA0; pin <= PIN_FRAME_POLARITY; ++pin) {
        if (gpio_get(pin)) levels |= 1u << (pin - PIN_DATA0);
    }
    sv_write_le32(status_packet + SV_STATUS_GPIO_OFFSET, levels);
}

static void queue_reconstructed_frame(void) {
    const uint32_t frame_sequence = sequence++;
    uint8_t slot_index;
    if (!queue_try_remove(&free_slots, &slot_index)) {
        ++dropped_frames;
    } else {
        frame_slot_t *slot = &frame_queue[slot_index];
        reconstruct_frame(slot);
        write_frame_header(slot->bytes, frame_sequence, time_us_64());
        queue_add_blocking(&ready_slots, &slot_index);
    }
}

static void begin_new_field(uint8_t polarity) {
    if (active_polarity >= 0) {
        if (!field_overflow && samples_in_field == FIELD_SAMPLES) {
            if (active_polarity != 0) {
                have_high_field = true;
            } else if (have_high_field) {
                queue_reconstructed_frame();
                have_high_field = false;
            }
        } else {
            // A short or long field means that framing was wrong. Discard any
            // pending pair and recover at this real polarity edge.
            have_high_field = false;
            ++dropped_frames;
        }
    }

    active_polarity = (int8_t)polarity;
    samples_in_field = 0;
    field_overflow = false;
}

static void consume_sample(uint8_t sample) {
    const uint8_t polarity = (sample >> 5u) & 1u;
    if (active_polarity < 0) {
        begin_new_field(polarity);
    } else if (polarity != (uint8_t)active_polarity &&
               samples_in_field >= FIELD_SAMPLES) {
        // FRAME_POLARITY is an LCD drive signal and can be electrically noisy
        // on a modified console. A real transition only occurs after all 6400
        // pixel groups; ignore shorter pulses instead of destroying framing.
        begin_new_field(polarity);
    }

    if (samples_in_field < FIELD_SAMPLES) {
        const uint8_t field_index = active_polarity != 0 ? 0u : 1u;
        field_samples[field_index][samples_in_field] = sample & 0x0fu;
    } else {
        field_overflow = true;
    }
    ++samples_in_field;
}

static void capture_service(void) {
    while (available_words() != 0) {
        const uint32_t word = raw_ring[consumer_word];
        consumer_word = (consumer_word + 1u) & RAW_RING_MASK;
        for (uint32_t i = 0; i < SAMPLES_PER_DMA_WORD; ++i) {
            consume_sample(sample_six_bits(word, i));
        }
    }
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
        status_tx_offset = 1; // 1-based so zero continues to mean idle.
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
        const uint32_t written = tud_vendor_write(status_packet + offset, amount);
        status_tx_offset += written;
        tud_vendor_flush();
        if (status_tx_offset - 1u == SV_STATUS_SIZE) status_tx_offset = 0;
        return;
    }

    const frame_slot_t *slot = &frame_queue[tx_slot];
    const uint32_t remaining = WIRE_FRAME_SIZE - tx_offset;
    const uint32_t amount = remaining < available ? remaining : available;
    const uint32_t written = tud_vendor_write(slot->bytes + tx_offset, amount);
    tx_offset += written;
    tud_vendor_flush();

    if (tx_offset == WIRE_FRAME_SIZE) {
        tx_offset = 0;
        tx_active = false;
        queue_add_blocking(&free_slots, &tx_slot);
    }
}

static void capture_init(void) {
    PIO pio = pio0;
    const uint sm = 0;
#if SV_USE_LINE_LATCH
    const uint offset = pio_add_program(pio, &supervision_capture_line_sync_program);
#else
    const uint offset = pio_add_program(pio, &supervision_capture_program);
#endif

    const uint last_input_pin = SV_USE_LINE_LATCH ? PIN_LINE_LATCH : PIN_FRAME_POLARITY;
    for (uint pin = PIN_DATA0; pin <= last_input_pin; ++pin) {
        pio_gpio_init(pio, pin);
        // Match the proven direct-GPIO capture implementations. This gives a
        // disconnected or marginal wire a defined low state instead of letting
        // it capacitively pick up PIXEL_CLOCK or adjacent LCD signals.
        gpio_pull_down(pin);
    }
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_DATA0,
                                   last_input_pin - PIN_DATA0 + 1u, false);

#if SV_USE_LINE_LATCH
    pio_sm_config config = supervision_capture_line_sync_program_get_default_config(offset);
#else
    pio_sm_config config = supervision_capture_program_get_default_config(offset);
#endif
    sm_config_set_in_pins(&config, PIN_DATA0);
    sm_config_set_in_shift(&config, true, true, 30);
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&config, 1.0f);
#if SV_USE_LINE_LATCH
    pio_sm_init(pio, sm,
                offset + supervision_capture_line_sync_offset_start, &config);
#else
    pio_sm_init(pio, sm, offset + supervision_capture_offset_start, &config);
#endif
    pio_sm_clear_fifos(pio, sm);

    dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_config = dma_channel_get_default_config(dma_channel);
    channel_config_set_transfer_data_size(&dma_config, DMA_SIZE_32);
    channel_config_set_read_increment(&dma_config, false);
    channel_config_set_write_increment(&dma_config, true);
    channel_config_set_ring(&dma_config, true, 15); // 2^15 bytes
    channel_config_set_dreq(&dma_config, pio_get_dreq(pio, sm, false));

    dma_channel_configure(dma_channel, &dma_config, raw_ring, &pio->rxf[sm],
                          UINT32_MAX, true);
    pio_sm_set_enabled(pio, sm, true);
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

int main(void) {
    // USB retains its independent 48 MHz clock. Core 0 captures and rebuilds
    // frames; core 1 services TinyUSB continuously so capture work cannot starve
    // the bulk endpoint at the native 50.81 fps rate.
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(200000, true);
    sleep_ms(10);

    stdio_init_all();
    queue_init(&free_slots, sizeof(uint8_t), FRAME_QUEUE_DEPTH);
    queue_init(&ready_slots, sizeof(uint8_t), FRAME_QUEUE_DEPTH);
    for (uint8_t slot = 0; slot < FRAME_QUEUE_DEPTH; ++slot) {
        queue_add_blocking(&free_slots, &slot);
    }
    capture_init();
    multicore_launch_core1(usb_core);

    while (true) {
        capture_service();
        tight_loop_contents();
    }
}

