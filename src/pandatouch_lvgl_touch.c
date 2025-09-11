#include "pandatouch_lvgl_touch.h"
#include "pandatouch_touch.h"
#include "esp_log.h"
#include <string.h>
#include "esp_heap_caps.h"

static const char *TAG = "PandaTouch::LVGL_Touch";

typedef struct
{
    int tp_w, tp_h;   // raw touch space (from controller)
    int scr_w, scr_h; // LVGL display resolution
} pt_lvgl_touch_ctx_t;

static pt_lvgl_touch_ctx_t s_ctx = {0};

/* Simple mapper from raw controller space -> LVGL display space */
static inline void pt_lvgl_touch_map_point(int rx, int ry, int *ox, int *oy)
{
    int x = rx;
    int y = ry;

    /* No swap/invert for this screen: coordinates map directly */

    // Scale to display
    // (int64 to avoid overflow if someone uses large coords)
    int32_t dx = (int32_t)x * (s_ctx.scr_w - 1) / (s_ctx.tp_w - 1 ? s_ctx.tp_w - 1 : 1);
    int32_t dy = (int32_t)y * (s_ctx.scr_h - 1) / (s_ctx.tp_h - 1 ? s_ctx.tp_h - 1 : 1);

    *ox = dx < 0 ? 0 : (dx >= s_ctx.scr_w ? s_ctx.scr_w - 1 : dx);
    *oy = dy < 0 ? 0 : (dy >= s_ctx.scr_h ? s_ctx.scr_h - 1 : dy);
}

/* LVGL read callback (v9 API) */
static void pt_lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{

    (void)indev;
    data->continue_reading = false;

    if (!pt_touch_i2c_ready())
    {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    pt_touch_event_t ev;
    if (!pt_touch_get_touch(&ev) || ev.number == 0)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    // Use the first point; extend to multi-touch if needed
    int mx, my;
    pt_lvgl_touch_map_point(ev.point[0].x, ev.point[0].y, &mx, &my);
    data->point.x = mx;
    data->point.y = my;
    data->state = LV_INDEV_STATE_PRESSED;

    // ESP_LOGI(TAG, "PT GT911 LVGL indev e %d,%d", ev.point[0].x, ev.point[0].y);
    // ESP_LOGI(TAG, "PT GT911 LVGL indev M %d,%d", mx, my);
}

/* Public init */
lv_indev_t *pt_lvgl_touch_init(lv_display_t *disp,
                               int tp_w, int tp_h)
{
    // Ensure GT911 is up (safe if already called)
    esp_err_t err = pt_touch_begin();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "pt_touch_begin failed: 0x%x", (int)err);
        return NULL;
    }

    // Resolve display size
    lv_display_t *use_disp = disp ? disp : lv_display_get_default();
    if (!use_disp)
    {
        ESP_LOGE(TAG, "No LVGL display found. Create a display first.");
        return NULL;
    }
    lv_coord_t hor = lv_display_get_horizontal_resolution(use_disp);
    lv_coord_t ver = lv_display_get_vertical_resolution(use_disp);

    // Save mapping context
    s_ctx.tp_w = (tp_w > 0) ? tp_w : PT_GT911_MAX_X;
    s_ctx.tp_h = (tp_h > 0) ? tp_h : PT_GT911_MAX_Y;
    s_ctx.scr_w = hor;
    s_ctx.scr_h = ver;

    // Create input device (LVGL v9 object API)
    lv_indev_t *indev = lv_indev_create();
    if (!indev)
    {
        ESP_LOGE(TAG, "lv_indev_create failed");
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, pt_lvgl_touch_read_cb);
    lv_indev_set_disp(indev, use_disp);

    ESP_LOGI(TAG, "PT GT911 LVGL indev registered (%ldx%ld touch -> %ldx%ld disp)",
             (long)s_ctx.tp_w, (long)s_ctx.tp_h, (long)s_ctx.scr_w, (long)s_ctx.scr_h);

    return indev;
}
