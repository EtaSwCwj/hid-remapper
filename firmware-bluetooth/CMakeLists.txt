cmake_minimum_required(VERSION 3.20.0)

set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

# This branch targets nRF52832 coin cell tags. The CI workflow only passes the
# nRF52840 board names, so the board and configuration are overridden here.
set(BOARD nrf52dk_nrf52832 CACHE STRING "" FORCE)
set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_tag.conf)

add_compile_definitions(PERSISTED_CONFIG_SIZE=2048)
# a single keyboard needs far fewer input states than the default 1024
add_compile_definitions(MAX_INPUT_STATES=256)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
include(${CMAKE_CURRENT_LIST_DIR}/gatt_dm_patch.cmake)
project(NONE)

set(REMAPPER_SRC ../firmware/src)

target_include_directories(app PRIVATE ${REMAPPER_SRC})

target_sources(app PRIVATE
    src/main.cc
    src/ble_out.c
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

# print the largest RAM consumers after linking (for fitting into 64 KB)
set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND sh -c "echo RAMTOP_BEGIN; ${CMAKE_NM} -S --size-sort -r -C ${CMAKE_BINARY_DIR}/zephyr/zephyr.elf | grep -i ' [bBdD] ' | head -45; echo RAMTOP_END")
