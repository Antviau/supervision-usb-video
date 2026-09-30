/* Standalone, input-only logic probe, not a replacement video firmware.
 * PIO/DMA sampling follows the Raspberry Pi pico-examples logic_analyser
 * method, with a finite capture and a separate USB request/reply transport.
 * https://github.com/raspberrypi/pico-examples/tree/master/pio/logic_analyser
 */
#include <string.h>
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "tusb.h"
#include "logic_probe_protocol.h"
#include "logic_probe.pio.h"

static uint32_t samples[SV_PROBE_MAX_PAYLOAD / 4u] __attribute__((aligned(4)));
static uint8_t reply[SV_PROBE_HEADER_SIZE];
static uint8_t command[SV_PROBE_COMMAND_SIZE];
static uint32_t command_used, reply_bytes, tx_offset;
static const PIO probe_pio = pio0;
static uint probe_sm, probe_dma, timing_offset, data_offset;

static void acquire(uint32_t request_id, uint32_t mode) {
    uint32_t flags = 0, count = 0, payload_bytes = 0;
    uint32_t rate = 0, base = 0, pins = 0, bits = 0, per_word = 0;
    const uint64_t armed_us = time_us_64();
    if (mode == SV_PROBE_MODE_TIMING || mode == SV_PROBE_MODE_DATA) {
        const bool timing = mode == SV_PROBE_MODE_TIMING;
        rate = timing ? 24000000u : 8000000u;
        base = timing ? 20u : 16u;
        pins = timing ? 3u : 7u;
        bits = timing ? 3u : 8u; // GP23 is captured but not used or driven.
        per_word = 32u / bits;
        count = (SV_PROBE_MAX_PAYLOAD / 4u) * per_word;
        const uint offset = timing ? timing_offset : data_offset;
        pio_sm_set_enabled(probe_pio, probe_sm, false);
        pio_sm_config c = timing ?
            supervision_timing_probe_program_get_default_config(offset) :
            supervision_data_probe_program_get_default_config(offset);
        sm_config_set_in_pins(&c, base);
        sm_config_set_in_shift(&c, true, true, per_word * bits);
        sm_config_set_clkdiv(&c, (float)clock_get_hz(clk_sys) / (2.0f * rate));
        pio_sm_init(probe_pio, probe_sm, offset, &c);
        pio_sm_clear_fifos(probe_pio, probe_sm);
        pio_sm_restart(probe_pio, probe_sm);
        pio_sm_clkdiv_restart(probe_pio, probe_sm);
        pio_sm_put(probe_pio, probe_sm, count - 1u);
        const uint32_t stall_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + probe_sm);
        probe_pio->fdebug = stall_bit;
        dma_channel_config dc = dma_channel_get_default_config(probe_dma);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, true);
        channel_config_set_dreq(&dc, pio_get_dreq(probe_pio, probe_sm, false));
        dma_channel_configure(probe_dma, &dc, samples, &probe_pio->rxf[probe_sm],
                              SV_PROBE_MAX_PAYLOAD / 4u, true);
        pio_sm_set_enabled(probe_pio, probe_sm, true);
        const uint64_t deadline = time_us_64() + 1000000u;
        while (dma_channel_is_busy(probe_dma) && tud_mounted() &&
               time_us_64() < deadline) tud_task();
        pio_sm_set_enabled(probe_pio, probe_sm, false);
        if (dma_channel_is_busy(probe_dma)) {
            dma_channel_abort(probe_dma);
            flags |= SV_PROBE_FLAG_TIMEOUT;
            count = 0;
        } else {
            payload_bytes = SV_PROBE_MAX_PAYLOAD;
        }
        if (probe_pio->fdebug & stall_bit) flags |= SV_PROBE_FLAG_RX_STALL;
        const uint32_t dma_errors = DMA_CH0_CTRL_TRIG_READ_ERROR_BITS |
                                    DMA_CH0_CTRL_TRIG_WRITE_ERROR_BITS;
        if (dma_hw->ch[probe_dma].ctrl_trig & dma_errors)
            flags |= SV_PROBE_FLAG_DMA_ERROR;
        dma_hw->ch[probe_dma].ctrl_trig |= dma_errors; // W1C error bits.
    } else if (mode != SV_PROBE_MODE_IDENTIFY) {
        flags |= SV_PROBE_FLAG_BAD_MODE;
    }

    uint32_t crc = 0xffffffffu;
    const uint8_t *bytes = (const uint8_t *)samples;
    for (uint32_t i = 0; i < payload_bytes; ++i) {
        crc = sv_probe_crc_update(crc, bytes[i]);
        if ((i & 1023u) == 0u) tud_task();
    }
    memcpy(reply, "SVW0", 4);
    sv_write_le16(reply + 4, 1);
    sv_write_le16(reply + 6, SV_PROBE_HEADER_SIZE);
    sv_write_le32(reply + 8, request_id);
    sv_write_le32(reply + 12, rate);
    sv_write_le32(reply + 16, count);
    sv_write_le32(reply + 20, payload_bytes);
    sv_write_le64(reply + 24, armed_us);
    reply[32] = (uint8_t)base;
    reply[33] = (uint8_t)pins;
    reply[34] = (uint8_t)bits;
    reply[35] = (uint8_t)per_word;
    sv_write_le32(reply + 36, flags);
    sv_write_le32(reply + 40, clock_get_hz(clk_sys));
    sv_write_le32(reply + 44, crc ^ 0xffffffffu);
    tx_offset = 0;
    reply_bytes = tud_mounted() ? SV_PROBE_HEADER_SIZE + payload_bytes : 0u;
}

void tud_umount_cb(void) {
    reply_bytes = tx_offset = command_used = 0;
}

int main(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(240000, true);
    // No pin used by the console is ever configured as an output.
    for (uint pin = 16; pin <= 22; ++pin) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
    }
    probe_sm = pio_claim_unused_sm(probe_pio, true);
    probe_dma = (uint)dma_claim_unused_channel(true);
    timing_offset = pio_add_program(probe_pio, &supervision_timing_probe_program);
    data_offset = pio_add_program(probe_pio, &supervision_data_probe_program);
    tusb_init();
    while (true) {
        tud_task();
        if (!tud_mounted()) continue;
        if (reply_bytes != 0) {
            const uint32_t available = tud_vendor_write_available();
            if (available != 0) {
                const bool header = tx_offset < SV_PROBE_HEADER_SIZE;
                const uint32_t left = header ? SV_PROBE_HEADER_SIZE - tx_offset :
                                               reply_bytes - tx_offset;
                const uint32_t chunk = left < available ? left : available;
                const uint8_t *source = header ? reply + tx_offset :
                    (const uint8_t *)samples + tx_offset - SV_PROBE_HEADER_SIZE;
                tx_offset += tud_vendor_write(source, chunk);
            }
            tud_vendor_flush();
            if (tx_offset == reply_bytes) reply_bytes = 0;
            continue;
        }
        if (tud_vendor_available()) {
            command_used += tud_vendor_read(command + command_used,
                                           sizeof(command) - command_used);
            if (command_used == sizeof(command)) {
                if (memcmp(command, "SVPC", 4) == 0)
                    acquire(sv_read_le32(command + 4), sv_read_le32(command + 8));
                command_used = 0;
            }
        }
    }
}
