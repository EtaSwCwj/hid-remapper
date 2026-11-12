// Minimal HID report descriptor parser: just enough to find keyboard and
// consumer control input fields. No dynamic memory.

#include "hid_parse.h"

#include <string.h>

#define MAX_FIELDS 24
#define MAX_LIST_USAGES 96
#define MAX_REPORT_IDS 8

#define PAGE_KEYBOARD 0x07
#define PAGE_CONSUMER 0x0C

struct field {
    uint8_t report_id;
    uint16_t bit_offset;
    uint8_t bit_size;
    uint8_t count;
    bool is_array;
    uint16_t usage_page;
    // either a usage range...
    uint16_t usage_min;
    uint16_t usage_max;
    int32_t logical_min;
    // ...or an explicit list (variable fields with individual usages)
    uint8_t list_start;
    uint8_t list_count;
};

static struct field fields[MAX_FIELDS];
static int field_count;
static uint32_t list_usages[MAX_LIST_USAGES];
static int list_usage_count;

struct report_offset {
    uint8_t report_id;
    uint16_t bits;
};

static bool interesting_page(uint16_t page) {
    return (page == PAGE_KEYBOARD) || (page == PAGE_CONSUMER);
}

int hid_parse_descriptor(const uint8_t* desc, uint16_t len) {
    struct report_offset offsets[MAX_REPORT_IDS];
    int offset_count = 0;

    uint16_t usage_page = 0;
    int32_t logical_min = 0;
    uint8_t report_size = 0;
    uint8_t report_count = 0;
    uint8_t report_id = 0;

    // local state, reset after every main item
    uint32_t local_usages[32];
    int local_usage_count = 0;
    uint32_t usage_min = 0;
    uint32_t usage_max = 0;
    bool have_range = false;

    field_count = 0;
    list_usage_count = 0;
    memset(offsets, 0, sizeof(offsets));

    uint16_t i = 0;
    while (i < len) {
        uint8_t prefix = desc[i];

        if (prefix == 0xFE) {  // long item: skip
            if (i + 2 >= len) {
                break;
            }
            i += 3 + desc[i + 1];
            continue;
        }

        uint8_t size = prefix & 0x03;
        if (size == 3) {
            size = 4;
        }
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = (prefix >> 4) & 0x0F;

        if (i + 1 + size > len) {
            break;
        }

        uint32_t value = 0;
        for (uint8_t b = 0; b < size; b++) {
            value |= ((uint32_t) desc[i + 1 + b]) << (8 * b);
        }
        int32_t svalue = (int32_t) value;
        if (size == 1) {
            svalue = (int8_t) value;
        } else if (size == 2) {
            svalue = (int16_t) value;
        }

        i += 1 + size;

        if (type == 1) {  // global
            switch (tag) {
                case 0x0:
                    usage_page = (uint16_t) value;
                    break;
                case 0x1:
                    logical_min = svalue;
                    break;
                case 0x7:
                    report_size = (uint8_t) value;
                    break;
                case 0x8:
                    report_id = (uint8_t) value;
                    break;
                case 0x9:
                    report_count = (uint8_t) value;
                    break;
                default:
                    break;
            }
        } else if (type == 2) {  // local
            // a 4 byte usage carries its own usage page
            uint32_t full = (size == 4) ? value : HID_USAGE(usage_page, value);
            switch (tag) {
                case 0x0:
                    if (local_usage_count < (int) (sizeof(local_usages) / sizeof(local_usages[0]))) {
                        local_usages[local_usage_count++] = full;
                    }
                    break;
                case 0x1:
                    usage_min = full;
                    have_range = true;
                    break;
                case 0x2:
                    usage_max = full;
                    have_range = true;
                    break;
                default:
                    break;
            }
        } else if (type == 0) {  // main
            if (tag == 0x8) {  // input
                struct report_offset* ro = NULL;
                for (int k = 0; k < offset_count; k++) {
                    if (offsets[k].report_id == report_id) {
                        ro = &offsets[k];
                        break;
                    }
                }
                if ((ro == NULL) && (offset_count < MAX_REPORT_IDS)) {
                    ro = &offsets[offset_count++];
                    ro->report_id = report_id;
                    ro->bits = 0;
                }

                bool is_const = value & 0x01;
                bool is_variable = value & 0x02;
                uint16_t total_bits = (uint16_t) report_size * report_count;

                if ((ro != NULL) && !is_const && (field_count < MAX_FIELDS)) {
                    struct field f;
                    memset(&f, 0, sizeof(f));
                    f.report_id = report_id;
                    f.bit_offset = ro->bits;
                    f.bit_size = report_size;
                    f.count = report_count;
                    f.is_array = !is_variable;
                    f.logical_min = logical_min;

                    bool keep = false;

                    if (have_range) {
                        f.usage_page = usage_min >> 16;
                        f.usage_min = usage_min & 0xFFFF;
                        f.usage_max = usage_max & 0xFFFF;
                        keep = interesting_page(f.usage_page);
                    } else if (local_usage_count > 0) {
                        f.usage_page = local_usages[0] >> 16;
                        if (list_usage_count + local_usage_count <= MAX_LIST_USAGES) {
                            f.list_start = list_usage_count;
                            f.list_count = local_usage_count;
                            for (int k = 0; k < local_usage_count; k++) {
                                list_usages[list_usage_count++] = local_usages[k];
                                if (interesting_page(local_usages[k] >> 16)) {
                                    keep = true;
                                }
                            }
                        }
                    }

                    if (keep) {
                        fields[field_count++] = f;
                    }
                }

                if (ro != NULL) {
                    ro->bits += total_bits;
                }
            }

            // any main item resets the local state
            local_usage_count = 0;
            have_range = false;
            usage_min = 0;
            usage_max = 0;
        }
    }

    return field_count;
}

static uint32_t get_bits(const uint8_t* data, uint16_t len, uint16_t bit_offset, uint8_t bit_size) {
    uint32_t value = 0;
    for (uint8_t b = 0; b < bit_size; b++) {
        uint16_t bit = bit_offset + b;
        uint16_t byte = bit >> 3;
        if (byte >= len) {
            break;
        }
        if (data[byte] & (1 << (bit & 7))) {
            value |= (1UL << b);
        }
    }
    return value;
}

int hid_extract(uint8_t report_id, const uint8_t* data, uint16_t len, uint32_t* out, int max) {
    int n = 0;

    for (int k = 0; k < field_count; k++) {
        const struct field* f = &fields[k];
        if (f->report_id != report_id) {
            continue;
        }

        for (uint8_t e = 0; e < f->count; e++) {
            if ((f->bit_size == 0) || (f->bit_size > 32)) {
                break;
            }
            uint32_t value = get_bits(data, len, f->bit_offset + (uint16_t) e * f->bit_size, f->bit_size);
            uint32_t usage = 0;

            if (f->is_array) {
                // the value selects a usage
                if (f->list_count > 0) {
                    int32_t idx = (int32_t) value - f->logical_min;
                    if ((idx < 0) || (idx >= f->list_count)) {
                        continue;
                    }
                    usage = list_usages[f->list_start + idx];
                } else {
                    int32_t id = (int32_t) f->usage_min + ((int32_t) value - f->logical_min);
                    if ((id <= 0) || (id > f->usage_max)) {
                        continue;
                    }
                    usage = HID_USAGE(f->usage_page, id);
                }
            } else {
                // one element per usage; non-zero means active
                if (value == 0) {
                    continue;
                }
                if (f->list_count > 0) {
                    if (e >= f->list_count) {
                        continue;
                    }
                    usage = list_usages[f->list_start + e];
                } else {
                    uint32_t id = (uint32_t) f->usage_min + e;
                    if (id > f->usage_max) {
                        continue;
                    }
                    usage = HID_USAGE(f->usage_page, id);
                }
            }

            if (((usage & 0xFFFF) == 0) || !interesting_page(usage >> 16)) {
                continue;
            }
            if (n < max) {
                out[n++] = usage;
            }
        }
    }

    return n;
}
