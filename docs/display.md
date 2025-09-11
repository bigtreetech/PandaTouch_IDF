# PandaTouch Display API

The display module wraps LVGL and the underlying ESP-LCD panel and provides:

- initialization and backlight control,
- a small set of helpers to schedule work on the LVGL thread,
- safe locking primitives for accessing LVGL from other FreeRTOS tasks,
- accessors for the LVGL display and the raw esp_lcd panel handle.

Headers required by callers:

```c
#include "pandatouch_display.h"
#include "lvgl.h" // (for LVGL types if needed)
#include "esp_err.h" // (for `esp_err_t`)
```

## Types

- `pt_ui_fn_t` — function pointer type for scheduling work on the LVGL thread: `typedef void (*pt_ui_fn_t)(void *arg);`

- `pt_lv_render_method_t` — enum describing supported render methods (mirrors board Kconfig):
  - `PT_LV_RENDER_FULL_1`
  - `PT_LV_RENDER_FULL_2`
  - `PT_LV_RENDER_PARTIAL_1`
  - `PT_LV_RENDER_PARTIAL_2` (default)
  - `PT_LV_RENDER_PARTIAL_1_PSRAM`
  - `PT_LV_RENDER_PARTIAL_2_PSRAM`

## Render methods

The firmware exposes several LVGL render strategies selectable via Kconfig. Choose the one that best fits your memory and tear/performance trade-offs.

| Method            |                   Approx memory footprint | Recommended when...                                                          |
| ----------------- | ----------------------------------------: | ---------------------------------------------------------------------------- |
| `FULL_1`          |                  ~1x framebuffer in PSRAM | You have PSRAM and want full-frame rendering with minimal internal RAM usage |
| `FULL_2`          |                  ~2x framebuffer in PSRAM | You have abundant PSRAM and need double-buffering to avoid tearing           |
| `PARTIAL_1`       | small partial buffer (internal preferred) | Internal RAM available but PSRAM is scarce; memory efficient                 |
| `PARTIAL_2`       |   2x partial buffers (internal preferred) | Want smoother flushes with limited RAM usage                                 |
| `PARTIAL_1_PSRAM` |             small partial buffer in PSRAM | Internal RAM limited, PSRAM available; slightly slower flushes               |
| `PARTIAL_2_PSRAM` |               2x partial buffers in PSRAM | Balance between smoothness and PSRAM usage                                   |

Additional Kconfig knobs you may care about:

- `PT_LV_RENDER_PARTIAL_BUFFER_LINES` — number of vertical lines per partial buffer (higher = fewer flushes, more memory).
- `PT_LV_RENDER_BOUNCING_BUFFER_LINES` — number of scanlines in the bounce buffer (used by some drivers).

When building firmware for constrained devices, prefer PARTIAL\_\* variants with small `PT_LV_RENDER_PARTIAL_BUFFER_LINES`. If you have abundant PSRAM and want tear-free double-buffering choose FULL_2.

## Lifecycle

- `esp_err_t pt_display_init(void)`

  - Initialize the driver, LVGL bindings and the panel. Returns `ESP_OK` on success or an `esp_err_t` error code.

- `bool pt_backlight_set(uint32_t percent)`

  - Set backlight brightness (0–100). Returns `true` on success or `false` on error.
  - Note: the function updates an in-memory setting. Use `pt_backlight_get()` to read the current value.

- `uint32_t pt_backlight_get(void)`
  - Returns the current backlight setting (0–100) as stored in memory.

## LVGL helpers and thread-safety

- `void pt_display_schedule_ui(pt_ui_fn_t fn, void *arg)`

  - Schedule `fn(arg)` to run on the LVGL thread (internally uses `lv_async_call` or equivalent). Use this when you need to update LVGL objects from other tasks.

- `lv_display_t *pt_get_display(void)`

  - Returns the pointer to the created LVGL display object (valid after successful `pt_display_init`).

- `esp_lcd_panel_handle_t pt_get_panel(void)`

  - Returns the underlying ESP-LCD panel handle if you need to call vendor-specific panel APIs.

- `void pt_lvgl_lock(void)` and `void pt_lvgl_unlock(void)`

  - Use these to protect LVGL calls from other tasks.

- `PT_LVGL_SCOPE_LOCK()` — a convenience macro that provides RAII-like scope locking. Example:

```c
// Safe block from a non-LVGL task
PT_LVGL_SCOPE_LOCK() {
    lv_label_set_text(my_label, "Hello from task");
    lv_obj_align(my_label, LV_ALIGN_CENTER, 0, 0);
}
```

This macro expands to a for-loop that acquires the lock and releases it at the end of the scope.

### Thread-safety contract

- `pt_display_schedule_ui()` is the recommended way to execute code on the LVGL thread.
- For short, synchronous operations from other tasks you may use `PT_LVGL_SCOPE_LOCK()`.
- Do not call LVGL APIs from arbitrary tasks without using the lock or the scheduler helper.

## Examples

1. Initialize display and set backlight

```c
if (pt_display_init() != ESP_OK) {
    printf("display init failed\n");
    return;
}

// set backlight to 80%
bool ok = pt_backlight_set(80);
if (!ok) {
  printf("backlight set failed\n");
}

// read current value
uint32_t current = pt_backlight_get();
printf("backlight %%u\n", (unsigned)current);
```

2. Schedule UI update from another FreeRTOS task

```c
void my_task(void *arg) {
    // Update some state then schedule UI refresh
    pt_display_schedule_ui(
        [](void *a){
            // this runs on LVGL thread
            lv_obj_t *lbl = lv_label_create(lv_scr_act());
            lv_label_set_text(lbl, "Updated from task");
        },
        NULL);
}
```

3. Safe LVGL access from background tasks

```c
// assume my_label is a global lv_obj_t*
void update_label_from_task(const char *text) {
    PT_LVGL_SCOPE_LOCK() {
        lv_label_set_text(my_label, text);
    }
}
```

4. Read panel handle for advanced usage

```c
esp_lcd_panel_handle_t panel = pt_get_panel();
if (panel) {
    // call advanced panel APIs (vendor-specific)
}
```

5. Sample app: `examples/display_sample.c`

- A small sample that demonstrates initialization, simple UI creation and safe cross-task updates.

- What it shows:

  - `pt_display_init()` and basic error handling
  - scheduling UI work with `pt_display_schedule_ui()`
  - using `PT_LVGL_SCOPE_LOCK()` to mutate LVGL objects from a background task
