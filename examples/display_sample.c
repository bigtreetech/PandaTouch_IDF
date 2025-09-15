// Simple sample app demonstrating pt_display_init, LVGL setup and
// usage of pt_display_schedule_ui and PT_LVGL_SCOPE_LOCK().

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pandatouch_display.h"

static lv_obj_t *g_label = NULL;

// Runs on LVGL thread: create initial UI
static void ui_create(void *arg)
{
    (void)arg;
    lv_obj_t *scr = lv_scr_act();
    g_label = lv_label_create(scr);
    lv_label_set_text(g_label, "Hello PandaTouch");
    lv_obj_align(g_label, LV_ALIGN_CENTER, 0, 0);
}

// Called from background task; will run on LVGL thread
static void ui_update_async(void *arg)
{
    (void)arg;
    if (!g_label)
    {
        return;
    }
    lv_label_set_text(g_label, "Updated via schedule_ui");
}

// Background task that schedules an LVGL update
static void bg_task(void *arg)
{
    (void)arg;
    // wait for display to be ready
    vTaskDelay(pdMS_TO_TICKS(2000));

    // schedule work on LVGL thread
    pt_display_schedule_ui(ui_update_async, NULL);

    // demonstrate using PT_LVGL_SCOPE_LOCK from a non-LVGL task
    vTaskDelay(pdMS_TO_TICKS(500));
    PT_LVGL_SCOPE_LOCK()
    {
        if (g_label)
        {
            lv_label_set_text(g_label, "Updated via scope lock");
        }
    }

    // exit
    vTaskDelete(NULL);
}

extern "C" void app_main(void)
{
    if (pt_display_init() != ESP_OK)
    {
        printf("pt_display_init failed\n");
        return;
    }

    // Schedule initial UI creation on LVGL thread
    pt_display_schedule_ui(ui_create, NULL);

    // Start background task that interacts with UI safely
    xTaskCreate(bg_task, "bg_task", 4096, NULL, 5, NULL);
}
