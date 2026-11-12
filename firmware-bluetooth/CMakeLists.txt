cmake_minimum_required(VERSION 3.20.0)

set(BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR})

# The CI workflow only knows the two nRF52840 board names. The XIAO build is
# the development target; the other job is redirected to an nRF52832 board to
# check that the firmware fits the coin cell tags.
if(BOARD STREQUAL "adafruit_feather_nrf52840")
    set(BOARD nrf52dk_nrf52832 CACHE STRING "" FORCE)
    set(CONF_FILE ${CMAKE_CURRENT_LIST_DIR}/prj_tag.conf)
else()
    set(CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/prj_tag.conf;${CMAKE_CURRENT_LIST_DIR}/xiao.conf")
endif()

set_property(GLOBAL APPEND PROPERTY extra_post_build_commands
    COMMAND sh ${CMAKE_CURRENT_LIST_DIR}/ramtop.sh)

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

target_sources(app PRIVATE
    src/tag_main.c
    src/ble_out.c
    src/hid_parse.c
    src/remap.c
)
