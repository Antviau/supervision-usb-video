#include <string.h>

#include "tusb.h"
#include "protocol.h"

enum {
    ITF_NUM_VENDOR = 0,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#define VENDOR_REQUEST_MICROSOFT 1u
#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define MS_OS_20_DESC_LEN 0xB2u

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    // USB 2.1 is required for the BOS descriptor used to bind WinUSB.
    .bcdUSB = 0x0210,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = SV_USB_VID,
    .idProduct = SV_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          0, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, SV_USB_EP_OUT, SV_USB_EP_IN, 64),
};

// Windows 8 and newer read this BOS capability and bind WinUSB without an INF
// file or Zadig. The GUID must match host/src/main.cpp.
static const uint8_t bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN,
                                VENDOR_REQUEST_MICROSOFT),
};

static const uint8_t ms_os_20_descriptor[] = {
    // Microsoft OS 2.0 descriptor-set header.
    U16_TO_U8S_LE(0x000A),
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset (configuration index 0).
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0x00, 0x00,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function subset (the vendor interface).
    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_VENDOR, 0x00,
    U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // Compatible ID: WINUSB.
    U16_TO_U8S_LE(0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ).
    U16_TO_U8S_LE(0x0084),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007),
    U16_TO_U8S_LE(0x002A),
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0,
    'a', 0, 'c', 0, 'e', 0, 'G', 0, 'U', 0, 'I', 0,
    'D', 0, 's', 0, 0, 0,
    U16_TO_U8S_LE(0x0050),
    // {A8B77A47-46EC-4DAD-9F56-8A4BF12D0EAE}\0\0
    '{', 0, 'A', 0, '8', 0, 'B', 0, '7', 0, '7', 0,
    'A', 0, '4', 0, '7', 0, '-', 0, '4', 0, '6', 0,
    'E', 0, 'C', 0, '-', 0, '4', 0, 'D', 0, 'A', 0,
    'D', 0, '-', 0, '9', 0, 'F', 0, '5', 0, '6', 0,
    '-', 0, '8', 0, 'A', 0, '4', 0, 'B', 0, 'F', 0,
    '1', 0, '2', 0, 'D', 0, '0', 0, 'E', 0, 'A', 0,
    'E', 0, '}', 0, 0, 0, 0, 0,
};

TU_VERIFY_STATIC(sizeof(ms_os_20_descriptor) == MS_OS_20_DESC_LEN,
                 "Incorrect Microsoft OS 2.0 descriptor size");

static const char *string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "Open Supervision Capture",
    "Supervision USB video capture",
    "SV0001",
    "Supervision frames",
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return configuration_descriptor;
}

uint8_t const *tud_descriptor_bos_cb(void) {
    return bos_descriptor;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7) {
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)ms_os_20_descriptor,
                                sizeof(ms_os_20_descriptor));
    }
    return false;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    static uint16_t descriptor[32];

    if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) {
        return NULL;
    }

    uint8_t count;
    if (index == 0) {
        memcpy(&descriptor[1], string_descriptors[0], 2);
        count = 1;
    } else {
        const char *text = string_descriptors[index];
        count = (uint8_t)strlen(text);
        if (count > 31) count = 31;
        for (uint8_t i = 0; i < count; ++i) descriptor[1 + i] = text[i];
    }

    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return descriptor;
}

