#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lvgl.h"
#include "draw/lv_image_decoder.h"
#include "draw/lv_image_decoder_private.h"

#include "pandatouch_display.h"
#include "pandatouch_msc.h"
#include "pandatouch_lvgl_msc.h"

static const char *TAG = "PandaTouch_display_slideshow";

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
    ESP_LOGI(TAG, "scan_dir_recursive: %s", path ? path : "(null)");
    int err = 0;
    pt_usb_dir_list_t *list = pt_usb_list_dir(path, &err);
    if (!list)
    {
        ESP_LOGW(TAG, "pt_usb_list_dir(\"%s\") returned NULL, err=%d", path ? path : "(null)", err);
        return;
    }

    ESP_LOGI(TAG, "pt_usb_list_dir: %s -> %zu entries", path, list->count);
    for (size_t i = 0; i < list->count; ++i)
    {
        pt_usb_dir_entry_t *e = &list->entries[i];
        if (e->is_hidden)
        {
            ESP_LOGD(TAG, "  skip hidden: %s", e->name ? e->name : "(no name)");
            continue;
        }

        const char *entry_path = e->path && e->path[0] ? e->path : NULL;
        char local_path_buf[512];

        if (!entry_path)
        {
            // build path = parent + "/" + name
            if (path && path[0] && path[strlen(path) - 1] == '/')
            {
                snprintf(local_path_buf, sizeof(local_path_buf), "%s%s", path, e->name ? e->name : "");
            }
            else
            {
                snprintf(local_path_buf, sizeof(local_path_buf), "%s/%s", path ? path : "", e->name ? e->name : "");
            }
            entry_path = local_path_buf;
        }

        ESP_LOGD(TAG, "  entry: name=\"%s\" path=\"%s\" is_dir=%d", e->name ? e->name : "(null)", entry_path, e->is_dir);

        if (e->is_dir)
        {
            scan_dir_recursive(entry_path, out_arr, out_cnt);
        }
        else
        {
            // check extension (case-insensitive)
            size_t L = strlen(entry_path);
            if (L >= 4)
            {
                const char *ext = entry_path + L - 4;
                if (strcasecmp(ext, ".png") == 0)
                {
                    char *dup = strdup(entry_path);
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
                            ESP_LOGI(TAG, "  collected PNG: %s", entry_path);
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
    scan_dir_recursive("/usb", &s_images, &s_images_count);

    s_have_images = (s_images_count > 0);
    ESP_LOGI(TAG, "Found %d png images on USB", (int)s_images_count);

    // If mounted and images found, ensure UI will be updated by slideshow task
    // Also schedule immediate display of the first image so the slideshow appears right away
    if (s_have_images)
    {
        ESP_LOGI(TAG, "Scheduling immediate display of first image: %s", s_images[0]);
        pt_display_schedule_ui(ui_set_image_arg, (void *)s_images[0]);
    }
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
    lv_coord_t disp_h = lv_display_get_vertical_resolution(pt_get_display());
    lv_obj_set_size(s_img, 400, 400);
    lv_obj_align(s_img, LV_ALIGN_TOP_LEFT, 0, 0);
    // add a border to see the image area
    lv_obj_set_style_border_color(s_img, lv_color_hex(0xff0000), 0);
    lv_obj_set_style_border_width(s_img, 2, 0);

    static const lv_style_prop_t slider_props[] = {LV_STYLE_BG_COLOR, 0};
    static lv_style_transition_dsc_t slider_transition_dsc;
    lv_style_transition_dsc_init(&slider_transition_dsc, slider_props, lv_anim_path_linear, 300, 0, NULL);

    static lv_style_t slider_style_main;
    static lv_style_t slider_style_indicator;
    static lv_style_t slider_style_knob;
    static lv_style_t slider_style_pressed_color;

    lv_style_init(&slider_style_main);
    lv_style_set_bg_opa(&slider_style_main, LV_OPA_COVER);
    lv_style_set_bg_color(&slider_style_main, lv_color_hex3(0xbbb));
    lv_style_set_radius(&slider_style_main, LV_RADIUS_CIRCLE);
    lv_style_set_pad_ver(&slider_style_main, -2); /*Makes the indicator larger*/

    lv_style_init(&slider_style_indicator);
    lv_style_set_bg_opa(&slider_style_indicator, LV_OPA_COVER);
    lv_style_set_bg_color(&slider_style_indicator, lv_palette_main(LV_PALETTE_CYAN));
    lv_style_set_radius(&slider_style_indicator, LV_RADIUS_CIRCLE);
    lv_style_set_transition(&slider_style_indicator, &slider_transition_dsc);

    lv_style_init(&slider_style_knob);
    lv_style_set_bg_opa(&slider_style_knob, LV_OPA_COVER);
    lv_style_set_bg_color(&slider_style_knob, lv_palette_main(LV_PALETTE_CYAN));
    lv_style_set_border_color(&slider_style_knob, lv_palette_darken(LV_PALETTE_CYAN, 3));
    lv_style_set_border_width(&slider_style_knob, 2);
    lv_style_set_radius(&slider_style_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&slider_style_knob, 6); /*Makes the knob larger*/
    lv_style_set_transition(&slider_style_knob, &slider_transition_dsc);

    lv_style_init(&slider_style_pressed_color);
    lv_style_set_bg_color(&slider_style_pressed_color, lv_palette_darken(LV_PALETTE_CYAN, 2));

    // Big slider on the right
    lv_obj_t *sld = lv_slider_create(scr);
    lv_obj_set_width(sld, 32);
    lv_obj_set_height(sld, (disp_h * 60) / 100);
    lv_obj_align(sld, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_slider_set_range(sld, 20, 100);
    lv_slider_set_value(sld, (int)pt_backlight_get(), LV_ANIM_OFF);

    lv_obj_add_style(sld, &slider_style_main, LV_PART_MAIN);
    lv_obj_add_style(sld, &slider_style_indicator, LV_PART_INDICATOR);
    lv_obj_add_style(sld, &slider_style_pressed_color, (lv_style_selector_t)((int)LV_PART_INDICATOR | (int)LV_STATE_PRESSED));
    lv_obj_add_style(sld, &slider_style_knob, LV_PART_KNOB);
    lv_obj_add_style(sld, &slider_style_pressed_color, (lv_style_selector_t)((int)LV_PART_KNOB | (int)LV_STATE_PRESSED));

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

    if (!(path && path[0]))
        return;

    ESP_LOGI(TAG, "ui_set_image_by_path called on LVGL thread for %s", path);

    lv_obj_clean(s_img);
    lv_img_set_src(s_img, path);
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
            const char *image = s_images[idx];
            ESP_LOGI(TAG, "slideshow: scheduling image %zu/%zu -> %s", idx + 1, s_images_count, image ? image : "(null)");
            /* quick pre-flight check: ensure the image path is readable from this task */
            FILE *f = fopen(image, "rb");
            if (!f)
            {
                ESP_LOGW(TAG, "slideshow: fopen failed for '%s' before scheduling (errno=%d)", image ? image : "(null)", errno);
            }
            else
            {
                fclose(f);
            }

            pt_display_schedule_ui(ui_set_image_arg, (void *)image);

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

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Starting display slideshow example");

    if (pt_display_init() != ESP_OK)
    {
        ESP_LOGE(TAG, "pt_display_init failed");
        return;
    }

    // Register USB callbacks and start host
    pt_usb_on_mount(usb_on_mount);
    pt_usb_on_unmount(usb_on_unmount);
    pt_usb_start();

    // Create initial UI on LVGL thread
    pt_display_schedule_ui(ui_create, NULL);

    // Start slideshow background task
    xTaskCreate(start_slideshow_task, "slideshow", 4096, NULL, 5, NULL);

    // Keep main task alive
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
