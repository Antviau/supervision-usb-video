cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)

project(supervision_viewer LANGUAGES CXX)

option(SUPERVISION_LCD_SUM3_DEFAULT "Default to LCD three-field grayscale reconstruction" OFF)
option(SUPERVISION_LOW_LATENCY_DEFAULT "Use smaller USB reads and immediate latest-frame painting" OFF)
option(SUPERVISION_BUFFERED_PAINT_DEFAULT "Compose each client-area image off-screen before presenting" OFF)

if(NOT WIN32)
    message(FATAL_ERROR "The host viewer currently targets Windows 10/11")
endif()

add_executable(supervision-viewer WIN32
    src/main.cpp
    src/supervision-viewer.manifest
)
target_compile_features(supervision-viewer PRIVATE cxx_std_17)
target_include_directories(supervision-viewer PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../common)
target_link_libraries(supervision-viewer PRIVATE
    setupapi
    winusb
    windowscodecs
    ole32
    shell32
)
target_compile_definitions(supervision-viewer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    SV_DEFAULT_LCD_SUM3=$<BOOL:${SUPERVISION_LCD_SUM3_DEFAULT}>
    SV_DEFAULT_LOW_LATENCY=$<BOOL:${SUPERVISION_LOW_LATENCY_DEFAULT}>
    SV_DEFAULT_BUFFERED_PAINT=$<BOOL:${SUPERVISION_BUFFERED_PAINT_DEFAULT}>
)

if(MSVC)
    target_compile_options(supervision-viewer PRIVATE /W4 /permissive- /utf-8)
endif()

set_target_properties(supervision-viewer PROPERTIES
    OUTPUT_NAME "SupervisionViewer"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

install(TARGETS supervision-viewer RUNTIME DESTINATION bin)

enable_testing()
add_executable(lcd-field-reconstruction-test tests/lcd_field_reconstruction_test.cpp)
target_compile_features(lcd-field-reconstruction-test PRIVATE cxx_std_17)
target_include_directories(lcd-field-reconstruction-test PRIVATE src ../common)
set_target_properties(lcd-field-reconstruction-test PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
if(MSVC)
    target_compile_options(lcd-field-reconstruction-test PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME lcd-field-reconstruction COMMAND lcd-field-reconstruction-test)

add_executable(lcd-bus-packing-test tests/lcd_bus_packing_test.cpp)
target_compile_features(lcd-bus-packing-test PRIVATE cxx_std_17)
target_include_directories(lcd-bus-packing-test PRIVATE ../common)
set_target_properties(lcd-bus-packing-test PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
if(MSVC)
    target_compile_options(lcd-bus-packing-test PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME lcd-bus-packing COMMAND lcd-bus-packing-test)

add_executable(supervision-logic-probe src/logic_probe_main.cpp)
target_compile_features(supervision-logic-probe PRIVATE cxx_std_17)
target_include_directories(supervision-logic-probe PRIVATE src ../common)
target_compile_definitions(supervision-logic-probe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(supervision-logic-probe PRIVATE setupapi winusb)
set_target_properties(supervision-logic-probe PROPERTIES
    OUTPUT_NAME "SupervisionLogicProbe"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
if(MSVC)
    target_compile_options(supervision-logic-probe PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(logic-probe-test tests/logic_probe_test.cpp)
target_compile_features(logic-probe-test PRIVATE cxx_std_17)
target_include_directories(logic-probe-test PRIVATE src ../common)
set_target_properties(logic-probe-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(logic-probe-test PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME logic-probe COMMAND logic-probe-test)

add_executable(lcd-sync-confirmation-test tests/lcd_sync_confirmation_test.cpp)
target_compile_features(lcd-sync-confirmation-test PRIVATE cxx_std_17)
target_include_directories(lcd-sync-confirmation-test PRIVATE src ../common)
set_target_properties(lcd-sync-confirmation-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(lcd-sync-confirmation-test PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME lcd-sync-confirmation COMMAND lcd-sync-confirmation-test)

add_executable(frame-slot-ring-test tests/frame_slot_ring_test.cpp)
target_compile_features(frame-slot-ring-test PRIVATE cxx_std_17)
target_include_directories(frame-slot-ring-test PRIVATE ../common)
set_target_properties(frame-slot-ring-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(frame-slot-ring-test PRIVATE /W4 /permissive- /utf-8)
endif()
add_test(NAME frame-slot-ring COMMAND frame-slot-ring-test)

add_executable(capture-cadence-test tests/capture_cadence_test.cpp)
target_compile_features(capture-cadence-test PRIVATE cxx_std_17)
target_include_directories(capture-cadence-test PRIVATE src)
set_target_properties(capture-cadence-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME capture-cadence COMMAND capture-cadence-test)

add_executable(buffered-gdi-frame-test tests/buffered_gdi_frame_test.cpp)
target_compile_features(buffered-gdi-frame-test PRIVATE cxx_std_17)
target_include_directories(buffered-gdi-frame-test PRIVATE src)
target_compile_definitions(buffered-gdi-frame-test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(buffered-gdi-frame-test PRIVATE gdi32 user32)
set_target_properties(buffered-gdi-frame-test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_test(NAME buffered-gdi-frame COMMAND buffered-gdi-frame-test)

