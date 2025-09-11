// Example: Display slideshow from USB-mounted PNGs with a large backlight slider
// - Initializes display and USB MSC
// - Shows USB mounted status and either an image (rotating every 5s) or a placeholder
// - Large slider on the right controls backlight 20..100%

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lvgl.h"
#include "pandatouch_display.h"
#include "pandatouch_msc.h"

static const char *TAG = "display_slideshow";

// UI objects (owned by LVGL thread)
static lv_obj_t *s_img = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_filename_lbl = NULL;

// Image list (owned by background task) - array of heap strdup'd strings
static char **s_images = NULL;
static size_t s_images_count = 0;
static volatile bool s_have_images = false;
static volatile bool s_usb_mounted = false;

// Forward decls
static void ui_create(void *arg);
static void ui_show_placeholder(void);
static void ui_set_image_by_path(const char *path, const char *name);
static void ui_set_image_arg(void *arg);
static void start_slideshow_task(void *arg);
static void scan_usb_for_pngs(void);
static void usb_on_mount(void);
static void usb_on_unmount(void);

// Recursively scan path using pt_usb_list_dir and collect *.png/*.PNG
static void scan_dir_recursive(const char *path, char ***out_arr, size_t *out_cnt)
{
    int err = 0;
    pt_usb_dir_list_t *list = pt_usb_list_dir(path, &err);
    if (!list)
        return;

    for (size_t i = 0; i < list->count; ++i)
    {
        pt_usb_dir_entry_t *e = &list->entries[i];
        if (e->is_dir)
        {
            scan_dir_recursive(e->path, out_arr, out_cnt);
        }
        else
        {
            const char *p = e->path ? e->path : "";
            size_t L = strlen(p);
            if (L >= 4)
            {
                const char *ext = p + L - 4;
                if (strcasecmp(ext, ".png") == 0)
                {
                    char *dup = strdup(p);
                    if (dup)
                    {
                        char **tmp = (char **)realloc(*out_arr, (*out_cnt + 1) * sizeof(char *));
                        if (!tmp)
                        {
                            free(dup);
                        }
                        else
                        {
                            *out_arr = tmp;
                            (*out_arr)[*out_cnt] = dup;
                            (*out_cnt)++;
                        }
                    }
                }
            }
        }
    }

    pt_usb_dir_list_free(list);
}

static void free_image_list(char **arr, size_t cnt)
{
    if (!arr)
        return;
    for (size_t i = 0; i < cnt; ++i)
    {
        free(arr[i]);
    }
    free(arr);
}

static void usb_on_mount(void)
{
    ESP_LOGI(TAG, "USB mounted callback");
    s_usb_mounted = true;
    // scan in background task
    scan_usb_for_pngs();
}

static void usb_on_unmount(void)
{
    ESP_LOGW(TAG, "USB unmounted callback");
    s_usb_mounted = false;
    // clear images
    free_image_list(s_images, s_images_count);
    s_images = NULL;
    s_images_count = 0;
    s_have_images = false;
    // update UI
    pt_display_schedule_ui((pt_ui_fn_t)ui_show_placeholder, NULL);
}

static void scan_usb_for_pngs(void)
{
    // free previous list
    free_image_list(s_images, s_images_count);
    s_images = NULL;
    s_images_count = 0;

    // start at root
    scan_dir_recursive("/", &s_images, &s_images_count);

    s_have_images = (s_images_count > 0);
    ESP_LOGI(TAG, "Found %d png images on USB", (int)s_images_count);

    // If mounted and images found, ensure UI will be updated by slideshow task
}

// LVGL-threaded UI creation
static void slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED)
    {
        lv_obj_t *t = (lv_obj_t *)lv_event_get_target(e);
        int v = lv_slider_get_value(t);
        pt_backlight_set((uint32_t)v);
    }
}

static void ui_create(void *arg)
{
    (void)arg;
    lv_obj_t *scr = lv_scr_act();

    // status label at top
    s_status_lbl = lv_label_create(scr);
    lv_label_set_text(s_status_lbl, "USB: Not mounted");
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 8);

    // filename label below status
    s_filename_lbl = lv_label_create(scr);
    lv_label_set_text(s_filename_lbl, "");
    lv_obj_align(s_filename_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    // main image area (left)
    s_img = lv_img_create(scr);
    // occupy left ~70% of screen
    lv_coord_t disp_w = lv_display_get_horizontal_resolution(pt_get_display());
    lv_coord_t disp_h = lv_display_get_vertical_resolution(pt_get_display());
    lv_obj_set_size(s_img, (disp_w * 70) / 100, (disp_h * 80) / 100);
    lv_obj_align(s_img, LV_ALIGN_LEFT_MID, 10, 0);

    // Big slider on the right
    lv_obj_t *sld = lv_slider_create(scr);
    lv_obj_set_width(sld, (disp_w * 20) / 100);
    lv_obj_set_height(sld, (disp_h * 60) / 100);
    lv_obj_align(sld, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_slider_set_range(sld, 20, 100);
    lv_slider_set_value(sld, (int)pt_backlight_get(), LV_ANIM_OFF);

    // (Optional) adjust slider visual style via styles if needed.

    // slider event -> set backlight (runs on LVGL thread)
    lv_obj_add_event_cb(sld, slider_event_cb, LV_EVENT_ALL, NULL);

    // initial placeholder
    ui_show_placeholder();
}

// Show placeholder / status when no images
static void ui_show_placeholder(void)
{
    if (!s_status_lbl)
        return;
    if (s_usb_mounted)
    {
        lv_label_set_text(s_status_lbl, "USB: Mounted");
    }
    else
    {
        lv_label_set_text(s_status_lbl, "USB: Not mounted");
    }

    if (!s_have_images)
    {
        // remove any previous children and show a simple text on image object
        lv_obj_clean(s_img);
        lv_obj_t *lbl = lv_label_create(s_img);
        lv_label_set_text(lbl, "No images\nInsert USB with PNGs");
        lv_obj_center(lbl);
        lv_obj_align(s_img, LV_ALIGN_LEFT_MID, 10, 0);
        lv_label_set_text(s_filename_lbl, "");
    }
}

static void ui_set_image_by_path(const char *path, const char *name)
{
    if (!s_img)
        return;

    // update status/filename
    if (s_status_lbl)
        lv_label_set_text(s_status_lbl, s_usb_mounted ? "USB: Mounted" : "USB: Not mounted");
    if (s_filename_lbl)
        lv_label_set_text(s_filename_lbl, name ? name : "");

    // Attempt to set image source to file path
    if (path && path[0])
    {
        // remove any previous children (e.g. placeholder labels)
        lv_obj_clean(s_img);
        // LVGL image decoder will attempt to load the file. If it fails, behavior
        // depends on LVGL config. We still try to set the src to the absolute path.
        lv_img_set_src(s_img, path);
    }
}

static void ui_set_image_arg(void *arg)
{
    const char *path = (const char *)arg;
    const char *name = NULL;
    if (path)
    {
        const char *p = strrchr(path, '/');
        name = p ? p + 1 : path;
    }
    ui_set_image_by_path(path, name);
}

// Background slideshow task: cycles through s_images every 5s when mounted
static void start_slideshow_task(void *arg)
{
    (void)arg;
    size_t idx = 0;
    while (1)
    {
        if (s_usb_mounted && s_have_images && s_images_count > 0)
        {
            // clamp idx
            if (idx >= s_images_count)
                idx = 0;

            // schedule UI update on LVGL thread
            const char *p = s_images[idx];
            const char *name = p ? strrchr(p, '/') : NULL;
            if (name)
                name++;
            pt_display_schedule_ui(ui_set_image_arg, (void *)p);

            // advance
            idx = (idx + 1) % s_images_count;
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        else
        {
            // no images: update UI placeholder occasionally
            pt_display_schedule_ui((pt_ui_fn_t)ui_show_placeholder, NULL);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting display slideshow example");

    if (pt_display_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "pt_display_init failed");
        return;
    }

    // Create initial UI on LVGL thread
    pt_display_schedule_ui(ui_create, NULL);

    // Register USB callbacks and start host
    pt_usb_on_mount(usb_on_mount);
    pt_usb_on_unmount(usb_on_unmount);
    pt_usb_start();

    // Start slideshow background task
    xTaskCreate(start_slideshow_task, "slideshow", 4096, NULL, 5, NULL);

    // Keep main task alive
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
