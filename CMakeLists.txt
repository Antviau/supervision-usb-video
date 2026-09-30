cmake_minimum_required(VERSION 3.20)

project(supervision_usb_capture NONE)

enable_testing()

option(SUPERVISION_BUILD_HOST "Build the PC viewer" ON)

if(SUPERVISION_BUILD_HOST)
    add_subdirectory(host)
endif()

