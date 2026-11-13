#ifndef _KEYMAP_H_
#define _KEYMAP_H_

// The key layout. Edit this file, rebuild and flash to change the mapping.

#include "hid_parse.h"

#define KEY(id) HID_USAGE(0x07, id)
#define CONSUMER(id) HID_USAGE(0x0C, id)

// keyboard page usages
#define K_3 0x20
#define K_A 0x04
#define K_D 0x07
#define K_E 0x08
#define K_F 0x09
#define K_G 0x0A
#define K_R 0x15
#define K_S 0x16
#define K_W 0x1A
#define K_BACKSPACE 0x2A
#define K_CAPS_LOCK 0x39
#define K_DELETE_FORWARD 0x4C
#define K_RIGHT 0x4F
#define K_LEFT 0x50
#define K_DOWN 0x51
#define K_UP 0x52
#define K_LEFT_CTRL 0xE0
#define K_LEFT_SHIFT 0xE1
#define K_LEFT_ALT 0xE2
#define K_LEFT_GUI 0xE3
#define K_RIGHT_CTRL 0xE4
#define K_RIGHT_SHIFT 0xE5
#define K_RIGHT_ALT 0xE6
#define K_RIGHT_GUI 0xE7

// consumer page usages
#define C_GLOBE 0x029D

#define LAYER0 0x01  // normal typing
#define LAYER1 0x02  // while the layer key is held
#define ALL_LAYERS (LAYER0 | LAYER1)

// Holding this key activates layer 1. The key itself is not sent to the host.
#define LAYER1_KEY KEY(K_RIGHT_ALT)

struct mapping {
    uint32_t source;
    uint8_t layers;
    uint32_t target[2];  // up to two usages are sent while the source is held
};

// A key that has no entry for the active layer is passed through unchanged.
static const struct mapping keymap[] = {
    // right Option layer: arrows, line start/end, delete
    { KEY(K_S), LAYER1, { KEY(K_LEFT), 0 } },
    { KEY(K_D), LAYER1, { KEY(K_DOWN), 0 } },
    { KEY(K_F), LAYER1, { KEY(K_RIGHT), 0 } },
    { KEY(K_E), LAYER1, { KEY(K_UP), 0 } },
    { KEY(K_A), LAYER1, { KEY(K_LEFT_GUI), KEY(K_LEFT) } },
    { KEY(K_G), LAYER1, { KEY(K_LEFT_GUI), KEY(K_RIGHT) } },
    { KEY(K_W), LAYER1, { KEY(K_BACKSPACE), 0 } },
    { KEY(K_R), LAYER1, { KEY(K_DELETE_FORWARD), 0 } },

    // Globe (fn) key works as Command, Command keys switch the input language
    { CONSUMER(C_GLOBE), ALL_LAYERS, { KEY(K_LEFT_GUI), 0 } },
    { KEY(K_LEFT_GUI), ALL_LAYERS, { KEY(K_CAPS_LOCK), 0 } },
    { KEY(K_RIGHT_GUI), ALL_LAYERS, { KEY(K_CAPS_LOCK), 0 } },
};

#endif
