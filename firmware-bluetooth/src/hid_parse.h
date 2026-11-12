#ifndef _HID_PARSE_H_
#define _HID_PARSE_H_

#include <stdbool.h>
#include <stdint.h>

// A usage is stored as (usage page << 16) | usage id.
#define HID_USAGE(page, id) (((uint32_t) (page) << 16) | (uint32_t) (id))

// Parses a HID report descriptor and remembers where the keyboard (page 0x07)
// and consumer (page 0x0C) input fields are. Returns the number of fields kept.
int hid_parse_descriptor(const uint8_t* desc, uint16_t len);

// Extracts the usages that are active in one input report.
// Returns the number of usages written to out.
int hid_extract(uint8_t report_id, const uint8_t* data, uint16_t len, uint32_t* out, int max);

#endif
