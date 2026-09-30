#ifndef SUPERVISION_LOGIC_PROBE_PROTOCOL_H
#define SUPERVISION_LOGIC_PROBE_PROTOCOL_H

#include "protocol.h"

#define SV_PROBE_COMMAND_SIZE 12u
#define SV_PROBE_HEADER_SIZE 48u
#define SV_PROBE_MAX_PAYLOAD 196608u
#define SV_PROBE_MODE_IDENTIFY 0u
#define SV_PROBE_MODE_TIMING 1u
#define SV_PROBE_MODE_DATA 2u
#define SV_PROBE_FLAG_TIMEOUT 1u
#define SV_PROBE_FLAG_RX_STALL 2u
#define SV_PROBE_FLAG_DMA_ERROR 4u
#define SV_PROBE_FLAG_BAD_MODE 8u

// Command: SVPC, request_id LE32, mode LE32.
// Reply: SVW0, version LE16, size LE16, request_id LE32, sample_hz LE32,
// sample_count LE32, payload_bytes LE32, arm_timestamp_us LE64,
// pin_base U8, pin_count U8, sample_bits U8, samples_per_word U8,
// flags LE32, system_clock_hz LE32, payload_crc32 LE32.
// Samples shift right into the PIO ISR. For three pins, ten samples occupy
// bits 2..31 of each word (the two least significant bits are padding).
static inline uint32_t sv_probe_sample(const uint8_t *payload, uint32_t index,
                                       uint32_t bits, uint32_t per_word) {
    const uint32_t word = sv_read_le32(payload + (index / per_word) * 4u);
    const uint32_t shift = 32u - per_word * bits + (index % per_word) * bits;
    return (word >> shift) & ((1u << bits) - 1u);
}

static inline uint32_t sv_probe_crc_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (uint32_t bit = 0; bit < 8; ++bit)
        crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
    return crc;
}

#endif
