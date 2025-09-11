# PandaTouch LVGL touch integration

This document describes the LVGL glue in `src/pandatouch_lvgl_touch.c` and the public initializer declared in `include/pandatouch_lvgl_touch.h`.

It focuses on how the driver maps raw GT911 touch coordinates into LVGL's input device API, the configuration flags (`swap_xy`, `invert_x`, `invert_y`), and how to use the initializer safely from application code.

## Purpose

- Provide a single-call helper to register a pointer-type LVGL input device backed by the board's touch controller.
- Centralize coordinate mapping and clamping so LVGL consumers don't reimplement touch-space-to-display-space math.

## Public API

- `lv_indev_t *pt_lvgl_touch_init(lv_display_t *disp, int tp_w, int tp_h);`

  - `disp` — optional LVGL display to bind the input to. If `NULL`, the default LVGL display is used.
  - `tp_w`, `tp_h` — touch controller logical resolution. Pass `0` or non-positive values to use sensible driver defaults (e.g., GT911 max values).

  - Returns: pointer to the created `lv_indev_t` on success, otherwise `NULL`.

## Runtime behavior

- `pt_lvgl_touch_init()` internally calls `pt_touch_begin()` to ensure the low-level touch driver is initialized. If that call fails the initializer returns `NULL`.
- The LVGL read callback checks `pt_touch_i2c_ready()` and calls `pt_touch_get_touch()` to obtain a `pt_touch_event_t` snapshot. If there's no touch or the controller reports no new data, the read callback reports `LV_INDEV_STATE_RELEASED`.
- When a touch is present the first reported point is mapped and the callback reports `LV_INDEV_STATE_PRESSED` with coordinates filled in `data->point`.

Note: the normal startup path calls this for you — `pt_display_init()` invokes `pt_lvgl_touch_init(pt_disp, 800, 480)` during initialization, so you usually don't need to call `pt_lvgl_touch_init()` manually unless you want different parameters or explicit control over touch registration.

## Examples

1. Basic initialization (use default display):

```c
lv_indev_t *indev = pt_lvgl_touch_init(NULL, 800, 480);
if (!indev) {
    ESP_LOGE("app", "pt_lvgl_touch_init failed");
}
```

2. Rotated panel (touch sensor not rotated):

```c
// panel is 90 degrees rotated relative to touch sensor
lv_indev_t *indev = pt_lvgl_touch_init(pt_get_display(), 480, 800);
```

3. Use from a background task (schedule a UI update):

```c
void on_new_state(void *arg) {
    // runs on LVGL thread
}

// from a FreeRTOS task:
pt_display_schedule_ui(on_new_state, NULL);
```

## Thread-safety & integration notes

- The LVGL read callback runs in LVGL task context; don't call LVGL APIs from other tasks without using `PT_LVGL_SCOPE_LOCK()` or scheduling via `pt_display_schedule_ui()`.
- If you need multi-touch support in LVGL, extend the glue to surface multiple points or implement a gesture layer that converts raw multi-touch into pointer+gesture events.
- The mapping flags should match any calibration or orientation settings used by the display module to keep touch and visual coordinates aligned.

## Troubleshooting

- If the function returns `NULL`, check logs for `pt_touch_begin failed` (I2C/address/probe problems).
- If coordinates are mirrored/rotated, toggle `swap_xy`, `invert_x`, and `invert_y` until they align, or compute correct values from your board wiring and panel orientation.

## Where to look in code

- LVGL glue: `src/pandatouch_lvgl_touch.c` (mapping, read callback, initialization)
- Low-level touch driver: `src/pandatouch_touch.c` and `include/pandatouch_touch.h`

---

File: `src/pandatouch_lvgl_touch.c`

This doc mirrors the style used in `docs/display.md` and focuses on LVGL integration.
