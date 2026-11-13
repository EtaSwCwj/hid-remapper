// Fixed-table key remapper. Keeps track of what is held on the keyboard,
// applies keymap.h and produces the reports that ble_out sends to the host.

#include "remap.h"

#include <stdbool.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
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

LOG_MODULE_REGISTER(remap, LOG_LEVEL_INF);

// The keyboard's top row has keys that are not keys of their own: the
// keyboard presses a combination for them (Globe+Up for the app switcher,
// Globe+E for emoji, Command+Shift+3 for a screenshot). Those must reach the
// host unchanged, even though Globe and Command are swapped for the keys
// under the fingers. They can be told apart by timing: the keyboard sends the
// whole combination at once, a person doesn't.
//
// So when Globe or Command goes down, nothing is sent for a moment. If the
// rest of one of those combinations shows up within that time, the modifier
// is passed through as it is; otherwise it is remapped as usual.
#define COMBO_WINDOW_MS 40

enum { SWAP_GLOBE, SWAP_GUI, SWAP_COUNT };

static struct {
    bool held;
    bool pending;  // just pressed, not decided yet, nothing sent
    bool raw;      // part of a keyboard-made combination: pass through
    int64_t since;
} swaps[SWAP_COUNT];

static K_MUTEX_DEFINE(remap_mutex);

static void recompute(void);

static void window_work_fn(struct k_work* work) {
    k_mutex_lock(&remap_mutex, K_FOREVER);
    recompute();
    k_mutex_unlock(&remap_mutex);
}
static K_WORK_DELAYABLE_DEFINE(window_work, window_work_fn);

static int swap_index(uint32_t usage) {
    if (usage == CONSUMER(C_GLOBE)) {
        return SWAP_GLOBE;
    }
    if ((usage == KEY(K_LEFT_GUI)) || (usage == KEY(K_RIGHT_GUI))) {
        return SWAP_GUI;
    }
    return -1;
}

static bool is_modifier(uint32_t usage) {
    return (usage >= KEY(0xE0)) && (usage <= KEY(0xE7));
}

static bool is_held(uint32_t usage) {
    for (int s = 0; s < MAX_SLOTS; s++) {
        for (int i = 0; slots[s].used && (i < slots[s].count); i++) {
            if (slots[s].held[i] == usage) {
                return true;
            }
        }
    }
    return false;
}

// Number of held keys that are neither modifiers, Globe nor the layer key.
static int plain_keys_held(void) {
    int n = 0;
    for (int s = 0; s < MAX_SLOTS; s++) {
        for (int i = 0; slots[s].used && (i < slots[s].count); i++) {
            uint32_t usage = slots[s].held[i];
            if (!is_modifier(usage) && (swap_index(usage) < 0)) {
                n++;
            }
        }
    }
    return n;
}

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

static void build(uint8_t layer, uint8_t tap_mask, uint8_t* keyboard, uint8_t* consumer) {
    for (int s = 0; s < MAX_SLOTS; s++) {
        for (int i = 0; slots[s].used && (i < slots[s].count); i++) {
            uint32_t usage = slots[s].held[i];
            if (usage == LAYER1_KEY) {
                continue;
            }

            int swap = swap_index(usage);
            if (swap >= 0) {
                if (swaps[swap].raw) {
                    emit(usage, keyboard, consumer);
                    continue;
                }
                if (swaps[swap].pending) {
                    continue;
                }
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

    // a swapped key that was released before it was decided: a quick tap
    static const uint32_t tap_source[SWAP_COUNT] = { CONSUMER(C_GLOBE), KEY(K_LEFT_GUI) };
    for (int w = 0; w < SWAP_COUNT; w++) {
        if (!(tap_mask & BIT(w))) {
            continue;
        }
        for (size_t m = 0; m < ARRAY_SIZE(keymap); m++) {
            if ((keymap[m].source == tap_source[w]) && (keymap[m].layers & layer)) {
                for (int t = 0; t < 2; t++) {
                    if (keymap[m].target[t] != 0) {
                        emit(keymap[m].target[t], keyboard, consumer);
                    }
                }
            }
        }
    }
}

static void send(uint8_t layer, uint8_t tap_mask) {
    uint8_t keyboard[KEYBOARD_REPORT_SIZE] = { 0 };
    uint8_t consumer[CONSUMER_REPORT_SIZE] = { 0 };

    build(layer, tap_mask, keyboard, consumer);

    if (memcmp(keyboard, last_keyboard, sizeof(keyboard)) != 0) {
        memcpy(last_keyboard, keyboard, sizeof(keyboard));
        ble_out_send(REPORT_ID_KEYBOARD, keyboard, sizeof(keyboard));
    }
    if (memcmp(consumer, last_consumer, sizeof(consumer)) != 0) {
        memcpy(last_consumer, consumer, sizeof(consumer));
        ble_out_send(REPORT_ID_CONSUMER, consumer, sizeof(consumer));
    }
}

static void recompute(void) {
    int64_t now = k_uptime_get();
    uint8_t layer = is_held(LAYER1_KEY) ? LAYER1 : LAYER0;
    uint8_t tap_mask = 0;

    bool held_now[SWAP_COUNT] = {
        is_held(CONSUMER(C_GLOBE)),
        is_held(KEY(K_LEFT_GUI)) || is_held(KEY(K_RIGHT_GUI)),
    };
    bool shift = is_held(KEY(K_LEFT_SHIFT)) || is_held(KEY(K_RIGHT_SHIFT));
    int plain = plain_keys_held();

    for (int w = 0; w < SWAP_COUNT; w++) {
        if (held_now[w] && !swaps[w].held) {
            swaps[w].pending = true;
            swaps[w].raw = false;
            swaps[w].since = now;
            k_work_reschedule(&window_work, K_MSEC(COMBO_WINDOW_MS));
        } else if (!held_now[w] && swaps[w].held) {
            if (swaps[w].pending) {
                tap_mask |= BIT(w);
            }
            swaps[w].pending = false;
            swaps[w].raw = false;
        }
        swaps[w].held = held_now[w];

        if (!swaps[w].pending) {
            continue;
        }

        bool combo;
        if (w == SWAP_GLOBE) {
            combo = (plain == 1) && (is_held(KEY(K_UP)) || is_held(KEY(K_E)));
        } else {
            combo = (plain == 1) && shift && is_held(KEY(K_3));
        }

        if (combo) {
            swaps[w].raw = true;
            swaps[w].pending = false;
            LOG_INF("keyboard-made combination %d after %d ms", w, (int) (now - swaps[w].since));
        } else if ((plain > 0) || ((now - swaps[w].since) >= COMBO_WINDOW_MS)) {
            // an ordinary key, or enough time has passed: remap as usual
            swaps[w].pending = false;
        }
    }

    if (tap_mask != 0) {
        send(layer, tap_mask);
    }
    send(layer, 0);
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

#ifdef CONFIG_USB_DEVICE_STACK
    // debug build: show what the keyboard sends and when
    LOG_INF("rep %u len %u: %02x %02x %02x %02x", report_id, len, len > 0 ? data[0] : 0, len > 1 ? data[1] : 0,
            len > 2 ? data[2] : 0, len > 3 ? data[3] : 0);
#endif

    k_mutex_lock(&remap_mutex, K_FOREVER);
    slot->count = hid_extract(report_id, data, len, slot->held, MAX_HELD);
    recompute();
    k_mutex_unlock(&remap_mutex);
}

void remap_reset(void) {
    k_mutex_lock(&remap_mutex, K_FOREVER);
    memset(slots, 0, sizeof(slots));
    memset(swaps, 0, sizeof(swaps));
    recompute();
    k_mutex_unlock(&remap_mutex);
}
