
#pragma once
#include "lvgl.h"

/**
 * Create and register an LVGL pointer input device backed by PT_GT911.
 *
 * @param disp      Optional display to bind the input to (can be NULL).
 * @param tp_w      Touch controller logical width  (e.g., 800).
 * @param tp_h      Touch controller logical height (e.g., 480).
 *
 * @return lv_indev_t*  The created input device (or NULL on failure).
 */
lv_indev_t *pt_lvgl_touch_init(lv_display_t *disp,
                               int tp_w, int tp_h);
