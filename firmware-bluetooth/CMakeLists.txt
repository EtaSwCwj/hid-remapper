cmake_minimum_required(VERSION 3.20.0)

set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

add_compile_definitions(PERSISTED_CONFIG_SIZE=2048)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

# debug build: logs go out over USB CDC ACM, so remove the driver's own
# "ring buffer full" message, which otherwise floods the log recursively
set(_cdc_acm_src ${ZEPHYR_BASE}/subsys/usb/device/class/cdc_acm.c)
file(READ ${_cdc_acm_src} _cdc_acm_content)
string(REGEX REPLACE "LOG_[A-Z]+\\(\"Ring buffer full, drain buffer\"\\);" "" _cdc_acm_patched "${_cdc_acm_content}")
if(NOT "${_cdc_acm_patched}" STREQUAL "${_cdc_acm_content}")
    file(WRITE ${_cdc_acm_src} "${_cdc_acm_patched}")
    message(STATUS "patched cdc_acm.c")
else()
    message(WARNING "cdc_acm.c not patched")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/gatt_dm_patch.cmake)
project(NONE)

set(REMAPPER_SRC ../firmware/src)

target_include_directories(app PRIVATE ${REMAPPER_SRC})

target_sources(app PRIVATE
    src/main.cc
    ${REMAPPER_SRC}/config.cc
    ${REMAPPER_SRC}/crc.cc
    ${REMAPPER_SRC}/descriptor_parser.cc
    ${REMAPPER_SRC}/globals.cc
    ${REMAPPER_SRC}/interval_override.cc
    ${REMAPPER_SRC}/our_descriptor.cc
    ${REMAPPER_SRC}/quirks.cc
    ${REMAPPER_SRC}/remapper.cc
    ${REMAPPER_SRC}/ps_auth.cc
)
