#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pandatouch_display.h"
#include "pandatouch_msc.h"

// Tag for logging
static const char *TAG = "PandaTouch_msc_sample";

// Callback function called when USB storage is mounted
static void on_mount_cb(void)
{
    ESP_LOGI(TAG, "Mount callback fired");

    // List files and directories in the USB root
    int err = 0;
    pt_usb_dir_list_t *list = pt_usb_list_dir("/usb", &err);
    if (!list)
    {
        ESP_LOGE(TAG, "list dir failed: %d", err);
        return;
    }

    ESP_LOGI(TAG, "Root entries: %d", list->count);
    for (int i = 0; i < list->count; ++i)
    {
        pt_usb_dir_entry_t *e = &list->entries[i];
        ESP_LOGI(TAG, "%s %s %s %u",
                 e->is_dir ? "DIR" : "FILE",
                 e->is_hidden ? "H" : " ",
                 e->name ? e->name : "(null)",
                 (unsigned)e->size);
        if (e->path)
            ESP_LOGI(TAG, "  path: %s", e->path);
    }

    pt_usb_dir_list_free(list);

    // Simple write/read test on the USB drive
    const char *p = "/usb/sample.txt";
    const char *data = "Hello World\n";

    // Write data to a file
    int w = pt_usb_write(p, (const uint8_t *)data, strlen(data), false);
    if (w == 0)
    {
        ESP_LOGI(TAG, "wrote %s", p);

        // Read back the data from the file
        char buf[256];
        size_t got = 0;
        int r = pt_usb_read(p, buf, sizeof(buf) - 1, &got);
        if (r == 0)
        {
            buf[got] = '\0';
            ESP_LOGI(TAG, "read back: %s", buf);
        }
        else
        {
            ESP_LOGE(TAG, "read back failed: %d", r);
        }
        // Remove the file after test
        pt_usb_remove(p);
    }
    else
    {
        ESP_LOGE(TAG, "write failed: %d", w);
    }
}

// Main entry point for the application
void app_main(void)
{
    ESP_LOGI(TAG, "Starting MSC sample");

    // Initialize the display
    pt_display_init();

    // Register mount callback (will fire immediately if already mounted)
    pt_usb_on_mount(on_mount_cb);

    // Start USB MSC (Mass Storage Class) handling
    pt_usb_start();

    // Keep the main task alive (demo purpose)
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
