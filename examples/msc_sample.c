#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "pandatouch_msc.h"

static const char *TAG = "msc_sample";

static void on_mount_cb(void)
{
    ESP_LOGI(TAG, "Mount callback fired");
    int err = 0;
    pt_usb_dir_list_t *list = pt_usb_list_dir("/", &err);
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

    // simple write/read test
    const char *p = "/sample.txt";
    const char *data = "Hello World\n";
    int w = pt_usb_write(p, (const uint8_t *)data, strlen(data));
    if (w == 0)
    {
        ESP_LOGI(TAG, "wrote %s", p);
        char *buf = NULL;
        int r = pt_usb_read(p, &buf, &err);
        if (r >= 0 && buf)
        {
            ESP_LOGI(TAG, "read back: %s", buf);
            free(buf);
        }
        else
        {
            ESP_LOGE(TAG, "read back failed: %d", err);
        }
        // cleanup
        pt_usb_remove(p);
    }
    else
    {
        ESP_LOGE(TAG, "write failed: %d", w);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting MSC sample");
    // register mount callback (will fire immediately if already mounted)
    pt_usb_on_mount(on_mount_cb);
    pt_usb_start();

    // keep the main task alive
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
