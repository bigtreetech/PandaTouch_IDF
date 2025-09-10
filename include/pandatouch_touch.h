#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "pandatouch_board.h"

typedef struct
{
    uint8_t track_id;
    uint16_t x;
    uint16_t y;
    uint16_t size; // “pressure/size”
} pt_touch_point_t;

typedef struct
{
    uint8_t number; // 0..5
    pt_touch_point_t point[PT_GT911_MAX_POINTS];
} pt_touch_event_t;

/* --------- API --------- */
esp_err_t pt_touch_begin(void);
bool pt_touch_i2c_ready(void);
bool pt_touch_get_touch(pt_touch_event_t *ev);
