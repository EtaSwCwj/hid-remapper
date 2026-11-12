// Fixed-table key remapper. Keeps track of what is held on the keyboard,
// applies keymap.h and produces the reports that ble_out sends to the host.

#include "remap.h"

#include <stdbool.h>
#include <string.h>

#include <zephyr/sys/util.h>

#include "ble_out.h"
#include "hid_parse.h"
#include "keymap.h"

// These must match the report map in ble_out.c.
#define REPORT_ID_KEYBOARD 2
#define REPORT_ID_CONSUMER 3
#define KEYBOARD_REPORT_SIZE 16
#define CONSUMER_REPORT_SIZE 3

// Consumer usages in the order of their bits in the consumer report.
static const uint16_t consumer_bits[] = {
    0x00B5, 0x00B6, 0x00B7, 0x00CD, 0x00E2, 0x00E9, 0x00EA, 0x0030, 0x0040,
    0x006F, 0x0070, 0x0071, 0x00B8, 0x00CF, 0x0221, 0x0223, 0x029D,
};

// The keyboard sends several kinds of reports (keys, consumer keys, ...).
// What is held is tracked separately for each of them.
#define MAX_SLOTS 6
#define MAX_HELD 16

struct slot {
    bool used;
    uint8_t report_id;
    uint8_t count;
    uint32_t held[MAX_HELD];
};

static struct slot slots[MAX_SLOTS];

static uint8_t last_keyboard[KEYBOARD_REPORT_SIZE];
static uint8_t last_consumer[CONSUMER_REPORT_SIZE];

static void set_bit(uint8_t* buf, uint16_t bit) {
    buf[bit >> 3] |= (1 << (bit & 7));
}

static void emit(uint32_t usage, uint8_t* keyboard, uint8_t* consumer) {
    uint16_t page = usage >> 16;
    uint16_t id = usage & 0xFFFF;

    if (page == 0x07) {
        if ((id >= 0xE0) && (id <= 0xE7)) {
            set_bit(keyboard, id - 0xE0);
        } else if ((id >= 0x04) && (id <= 0x73)) {
            set_bit(keyboard, 8 + (id - 0x04));
        } else if ((id >= 0x87) && (id <= 0x8B)) {
            set_bit(keyboard, 120 + (id - 0x87));
        } else if ((id == 0x90) || (id == 0x91)) {
            set_bit(keyboard, 125 + (id - 0x90));
        }
    } else if (page == 0x0C) {
        for (size_t i = 0; i < ARRAY_SIZE(consumer_bits); i++) {
            if (consumer_bits[i] == id) {
                set_bit(consumer, i);
                break;
            }
        }
    }
}

static void recompute(void) {
    uint8_t keyboard[KEYBOARD_REPORT_SIZE] = { 0 };
    uint8_t consumer[CONSUMER_REPORT_SIZE] = { 0 };

    uint8_t layer = LAYER0;
    for (int s = 0; s < MAX_SLOTS; s++) {
        for (int i = 0; slots[s].used && (i < slots[s].count); i++) {
            if (slots[s].held[i] == LAYER1_KEY) {
                layer = LAYER1;
            }
        }
    }

    for (int s = 0; s < MAX_SLOTS; s++) {
        for (int i = 0; slots[s].used && (i < slots[s].count); i++) {
            uint32_t usage = slots[s].held[i];
            if (usage == LAYER1_KEY) {
                continue;
            }

            bool mapped = false;
            for (size_t m = 0; m < ARRAY_SIZE(keymap); m++) {
                if ((keymap[m].source == usage) && (keymap[m].layers & layer)) {
                    mapped = true;
                    for (int t = 0; t < 2; t++) {
                        if (keymap[m].target[t] != 0) {
                            emit(keymap[m].target[t], keyboard, consumer);
                        }
                    }
                }
            }
            if (!mapped) {
                emit(usage, keyboard, consumer);
            }
        }
    }

    if (memcmp(keyboard, last_keyboard, sizeof(keyboard)) != 0) {
        memcpy(last_keyboard, keyboard, sizeof(keyboard));
        ble_out_send(REPORT_ID_KEYBOARD, keyboard, sizeof(keyboard));
    }
    if (memcmp(consumer, last_consumer, sizeof(consumer)) != 0) {
        memcpy(last_consumer, consumer, sizeof(consumer));
        ble_out_send(REPORT_ID_CONSUMER, consumer, sizeof(consumer));
    }
}

void remap_handle_report(uint8_t report_id, const uint8_t* data, uint16_t len) {
    struct slot* slot = NULL;

    for (int s = 0; s < MAX_SLOTS; s++) {
        if (slots[s].used && (slots[s].report_id == report_id)) {
            slot = &slots[s];
            break;
        }
    }
    if (slot == NULL) {
        for (int s = 0; s < MAX_SLOTS; s++) {
            if (!slots[s].used) {
                slot = &slots[s];
                slot->used = true;
                slot->report_id = report_id;
                break;
            }
        }
    }
    if (slot == NULL) {
        return;
    }

    slot->count = hid_extract(report_id, data, len, slot->held, MAX_HELD);

    recompute();
}

void remap_reset(void) {
    memset(slots, 0, sizeof(slots));
    recompute();
}
