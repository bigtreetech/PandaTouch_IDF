
#pragma once
#include "lvgl.h"

/**
 * Create and register an LVGL pointer input device backed by PT_GT911.
 *
 * @param disp      Optional display to bind the input to (can be NULL).
 * @param tp_w      Touch controller logical width  (e.g., 800).
 * @param tp_h      Touch controller logical height (e.g., 480).
 * @param swap_xy   Swap X/Y (set true if panel is rotated 90/270° vs touch).
 * @param invert_x  Invert X axis after swap mapping.
 * @param invert_y  Invert Y axis after swap mapping.
 *
 * @return lv_indev_t*  The created input device (or NULL on failure).
 */
lv_indev_t *pt_lvgl_touch_init(lv_display_t *disp,
                               int tp_w, int tp_h,
                               bool swap_xy, bool invert_x, bool invert_y);
