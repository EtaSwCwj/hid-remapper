#ifndef _BATTERY_H_
#define _BATTERY_H_

#include <stdint.h>

// Measures the supply voltage (the coin cell on the tags) and publishes it
// through the Bluetooth Battery Service, so the host can show the level.
void battery_init(void);
uint16_t battery_millivolts(void);
uint8_t battery_percent(void);

#endif
