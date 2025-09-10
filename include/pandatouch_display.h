#pragma once
#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"

    /* ======= Public render method enum (mirrors Kconfig) ======= */
    typedef enum
    {
        PT_LV_RENDER_FULL_1 = 0,
        PT_LV_RENDER_FULL_2,
        PT_LV_RENDER_PARTIAL_1,
        PT_LV_RENDER_PARTIAL_2, /* default */
        PT_LV_RENDER_PARTIAL_1_PSRAM,
        PT_LV_RENDER_PARTIAL_2_PSRAM
    } pt_lv_render_method_t;

    /* ======= Lifecycle ======= */
    esp_err_t pt_display_init(void);
    int pt_backlight_set(uint32_t percent, bool save);

    /* ======= LVGL helpers ======= */
    /* Schedule a function to run on the LVGL thread (lv_async_call) */
    typedef void (*pt_ui_fn_t)(void *arg);
    void pt_display_schedule_ui(pt_ui_fn_t fn, void *arg);

    /* Get the created lv_display_t* (after pt_init) */
    lv_display_t *pt_get_display(void);

    /* Access the underlying esp_lcd panel if needed */
    esp_lcd_panel_handle_t pt_get_panel(void);

    /* Expose the lock macro so user code can safely touch LVGL from other tasks */
    void pt_lvgl_lock(void);
    void pt_lvgl_unlock(void);

/* Convenience RAII-ish scope macro */
#define PT_LVGL_SCOPE_LOCK()              \
    for (int _once = 1; _once; _once = 0) \
        for (pt_lvgl_lock(); _once; _once = (pt_lvgl_unlock(), 0))

#ifdef __cplusplus
}
#endif
