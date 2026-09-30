#ifndef SUPERVISION_PROTOCOL_H
#define SUPERVISION_PROTOCOL_H

#include <stdint.h>

#define SV_USB_VID                 0xCAFEu
#define SV_USB_PID                 0x4020u
#define SV_USB_INTERFACE           0u
#define SV_USB_EP_OUT              0x01u
#define SV_USB_EP_IN               0x81u

#define SV_PROTOCOL_VERSION        1u
#define SV_FRAME_HEADER_SIZE       40u
#define SV_FRAME_WIDTH             160u
#define SV_FRAME_HEIGHT            160u
#define SV_FRAME_PAYLOAD_SIZE      6400u
#define SV_LCD_FIELD_RATE_MILLIHZ  101626u
#define SV_FIELDS_PER_FRAME        2u
#define SV_SOURCE_RATE_MILLIHZ     (SV_LCD_FIELD_RATE_MILLIHZ / SV_FIELDS_PER_FRAME)

#define SV_HEADER_MAGIC_OFFSET     0u
#define SV_HEADER_VERSION_OFFSET   4u
#define SV_HEADER_SIZE_OFFSET      6u
#define SV_HEADER_SEQUENCE_OFFSET  8u
#define SV_HEADER_TIMESTAMP_OFFSET 12u
#define SV_HEADER_WIDTH_OFFSET     20u
#define SV_HEADER_HEIGHT_OFFSET    22u
#define SV_HEADER_PAYLOAD_OFFSET   24u
#define SV_HEADER_RATE_OFFSET      28u
#define SV_HEADER_DROPPED_OFFSET   32u
#define SV_HEADER_FLAGS_OFFSET     36u

#define SV_FRAME_FLAG_HIGH_FIELD_IS_MSB (1u << 0)

#define SV_STATUS_SIZE                  32u
#define SV_STATUS_VERSION_OFFSET        4u
#define SV_STATUS_SIZE_OFFSET           6u
#define SV_STATUS_TIMESTAMP_OFFSET      8u
#define SV_STATUS_CAPTURED_WORDS_OFFSET 16u
#define SV_STATUS_SEQUENCE_OFFSET       20u
#define SV_STATUS_DROPPED_OFFSET        24u
#define SV_STATUS_GPIO_OFFSET           28u

static inline uint16_t sv_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t sv_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t sv_read_le64(const uint8_t *p) {
    return (uint64_t)sv_read_le32(p) |
           ((uint64_t)sv_read_le32(p + 4) << 32);
}

static inline void sv_write_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static inline void sv_write_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static inline void sv_write_le64(uint8_t *p, uint64_t value) {
    sv_write_le32(p, (uint32_t)value);
    sv_write_le32(p + 4, (uint32_t)(value >> 32));
}

#endif

