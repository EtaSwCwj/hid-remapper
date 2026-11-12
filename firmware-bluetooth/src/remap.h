#ifndef _REMAP_H_
#define _REMAP_H_

#include <stdint.h>

// Feeds one input report from the keyboard into the remapper. The resulting
// keyboard/consumer state is sent to the host if it changed.
void remap_handle_report(uint8_t report_id, const uint8_t* data, uint16_t len);

// Forgets everything that is held down (keyboard disconnected) and tells the host.
void remap_reset(void);

#endif
