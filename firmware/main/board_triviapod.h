#ifndef BOARD_TRIVIAPOD_H
#define BOARD_TRIVIAPOD_H
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

bool triviapod_init(i2c_master_bus_handle_t bus);
bool triviapod_panel_reset(void);
void triviapod_panel_sleep(void);
bool triviapod_touch_init(void);
bool triviapod_touch_read(uint16_t *x, uint16_t *y, bool *down);
bool triviapod_touch_sleep(void);
bool triviapod_amp(bool on);
void triviapod_vibrate(unsigned ms);
void triviapod_motor_stop(void);
bool triviapod_feed_pressed(int64_t now);

#endif
