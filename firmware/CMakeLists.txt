cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
    else()
        message(FATAL_ERROR "Set PICO_SDK_PATH to the Raspberry Pi Pico SDK directory")
    endif()
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(supervision_capture_firmware C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

function(add_supervision_firmware target use_line_latch)
    add_executable(${target}
        src/main.c
        src/usb_descriptors.c
    )

    pico_generate_pio_header(${target}
        ${CMAKE_CURRENT_LIST_DIR}/src/supervision_capture.pio
    )

    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/../common
    )
    target_compile_definitions(${target} PRIVATE
        SV_USE_LINE_LATCH=${use_line_latch}
    )

    target_link_libraries(${target} PRIVATE
        pico_stdlib
        hardware_clocks
        hardware_dma
        hardware_pio
        hardware_vreg
        pico_multicore
        tinyusb_device
    )

    pico_enable_stdio_usb(${target} 0)
    pico_enable_stdio_uart(${target} 0)
    pico_add_extra_outputs(${target})
endfunction()

# The default artifact is the robust seven-wire build (pin 7 LINE_LATCH -> GP22).
add_supervision_firmware(supervision_capture 1)
add_supervision_firmware(supervision_capture_6wire 0)

# Six-wire polling build using the capture method proven by DutchMaker v2 and
# xrip.  It intentionally omits PIO, DMA, and LINE_LATCH from the capture path.
add_executable(supervision_capture_proven
    src/main_proven.c
    src/usb_descriptors.c
)
target_include_directories(supervision_capture_proven PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_link_libraries(supervision_capture_proven PRIVATE
    pico_stdlib
    hardware_clocks
    hardware_vreg
    pico_multicore
    tinyusb_device
)
pico_enable_stdio_usb(supervision_capture_proven 0)
pico_enable_stdio_uart(supervision_capture_proven 0)
pico_add_extra_outputs(supervision_capture_proven)

# Rejected on real hardware (more lines); retained for diagnostic comparison.
# Same proven field/line addressing and USB protocol.
# Only the per-clock packing and short-line RAM padding are optimized.
add_executable(supervision_capture_polling_fast
    src/main_proven.c
    src/usb_descriptors.c
)
target_include_directories(supervision_capture_polling_fast PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_compile_definitions(supervision_capture_polling_fast PRIVATE SV_POLLING_FAST=1)
target_link_libraries(supervision_capture_polling_fast PRIVATE
    pico_stdlib hardware_clocks hardware_vreg pico_multicore tinyusb_device
)
pico_enable_stdio_usb(supervision_capture_polling_fast 0)
pico_enable_stdio_uart(supervision_capture_polling_fast 0)
pico_add_extra_outputs(supervision_capture_polling_fast)

# Separate measured-spike qualification experiment. The reference build above
# keeps SV_SYNC_CONFIRM=0 and SV_POLLING_FAST=0, with unchanged capture semantics.
add_executable(supervision_capture_sync_confirm src/main_proven.c src/usb_descriptors.c)
target_include_directories(supervision_capture_sync_confirm PRIVATE src ../common)
target_compile_definitions(supervision_capture_sync_confirm PRIVATE SV_SYNC_CONFIRM=1)
target_link_libraries(supervision_capture_sync_confirm PRIVATE
    pico_stdlib hardware_clocks hardware_vreg pico_multicore tinyusb_device
)
pico_enable_stdio_usb(supervision_capture_sync_confirm 0)
pico_enable_stdio_uart(supervision_capture_sync_confirm 0)
pico_add_extra_outputs(supervision_capture_sync_confirm)

# Preserve sync qualification; keep header construction off the capture core
# and publish buffer ownership through bounded SPSC rings at field boundaries.
add_executable(supervision_capture_fast_field_handoff src/main_proven.c src/usb_descriptors.c)
target_include_directories(supervision_capture_fast_field_handoff PRIVATE src ../common)
target_compile_definitions(supervision_capture_fast_field_handoff PRIVATE
    SV_SYNC_CONFIRM=1 SV_FAST_FIELD_HANDOFF=1
)
target_link_libraries(supervision_capture_fast_field_handoff PRIVATE
    pico_stdlib hardware_clocks hardware_vreg pico_multicore tinyusb_device
)
pico_enable_stdio_usb(supervision_capture_fast_field_handoff 0)
pico_enable_stdio_uart(supervision_capture_fast_field_handoff 0)
pico_add_extra_outputs(supervision_capture_fast_field_handoff)

# Experimental three-field reference port.  Kept as a diagnostic target only:
# it is not the recommended USB capture build for the real console.
add_executable(supervision_capture_reference_exact
    src/main_reference_exact.c
    src/usb_descriptors.c
)
target_include_directories(supervision_capture_reference_exact PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/../common
)
target_link_libraries(supervision_capture_reference_exact PRIVATE
    pico_stdlib
    hardware_clocks
    hardware_vreg
    pico_multicore
    tinyusb_device
)
pico_enable_stdio_usb(supervision_capture_reference_exact 0)
pico_enable_stdio_uart(supervision_capture_reference_exact 0)
pico_add_extra_outputs(supervision_capture_reference_exact)

# Diagnostic-only, input-only waveform acquisition. Does not run video capture.
add_executable(supervision_logic_probe src/main_logic_probe.c src/usb_descriptors.c)
pico_generate_pio_header(supervision_logic_probe ${CMAKE_CURRENT_LIST_DIR}/src/logic_probe.pio)
target_include_directories(supervision_logic_probe PRIVATE src ../common)
target_link_libraries(supervision_logic_probe PRIVATE
    pico_stdlib hardware_clocks hardware_vreg hardware_pio hardware_dma tinyusb_device
)
pico_enable_stdio_usb(supervision_logic_probe 0)
pico_enable_stdio_uart(supervision_logic_probe 0)
pico_add_extra_outputs(supervision_logic_probe)

