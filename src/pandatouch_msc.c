// pt_usb_msc.c — ESP-IDF 5.1 ready

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include <inttypes.h>

#include "usb/usb_host.h"     // IDF 5.1: usb_host_* + flags
#include "usb/msc_host.h"     // IDF 5.1: MSC host core
#include "usb/msc_host_vfs.h" // IDF 5.1: VFS mount helper

#include "pandatouch_msc.h" // your header: types, macros, prototypes

#include <stdlib.h>

// -------- Config --------
#define TAG "pt_usb_msc"

#ifndef XUSB_MSC_HOST_TASK_STACK
#define XUSB_MSC_HOST_TASK_STACK 4096
#endif
#ifndef XUSB_MSC_EVENTS_TASK_STACK
#define XUSB_MSC_EVENTS_TASK_STACK 4096
#endif

// install retry behaviour
#ifndef XUSB_INSTALL_MAX_RETRIES
#define XUSB_INSTALL_MAX_RETRIES 5
#endif
#ifndef XUSB_INSTALL_RETRY_DELAY_MS
#define XUSB_INSTALL_RETRY_DELAY_MS 500
#endif

#ifdef ARDUINO
#include <Arduino.h>
#define xusb_delay_ms(ms) delay(ms)
#else
static inline void xusb_delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
#endif

// -------- State --------
static TaskHandle_t s_usb_events_task = NULL;
static TaskHandle_t s_msc_events_task = NULL;
static TaskHandle_t s_install_task = NULL;
static QueueHandle_t s_install_queue = NULL;

static msc_host_device_handle_t s_dev = NULL;
static msc_host_vfs_handle_t s_vfs = NULL;

static volatile bool s_mounted = false;
static pt_usb_msc_info_t s_info = {.state = XUSB_MSC_STATE_STOPPED};

// Forward decls
static void usb_host_events_task(void *arg);
static void msc_events_task(void *arg);
static void msc_cb(const msc_host_event_t *event, void *arg);
static void mount_vfs(msc_host_device_handle_t dev);
static void unmount_vfs(void);
static void install_device_task(void *arg);
static void self_test_list_root(void);
static void self_test_cb(const char *name, bool is_dir, size_t size, void *user);

// ========== Public API ==========

bool pt_usb_msc_start(void)
{
    if (s_info.state != XUSB_MSC_STATE_STOPPED)
        return true;

    // 1) Start USB Host library
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    // 2) Install MSC Host driver (we pump events in our own task)
    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = false,
        .callback = msc_cb,
        .callback_arg = NULL,
        .task_priority = 0,
        .stack_size = 0,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));

    // 3) Event loops
    xTaskCreate(usb_host_events_task, "usb_host_ev", XUSB_MSC_HOST_TASK_STACK, NULL, 5, &s_usb_events_task);
    xTaskCreate(msc_events_task, "msc_host_ev", XUSB_MSC_EVENTS_TASK_STACK, NULL, 5, &s_msc_events_task);

    // increase USB/MSC logging for diagnostics
    esp_log_level_set("USB_MSC", ESP_LOG_DEBUG);
    esp_log_level_set("USB_HOST", ESP_LOG_DEBUG);

    // install worker queue and task (small, bounded)
    if (!s_install_queue)
    {
        s_install_queue = xQueueCreate(4, sizeof(uint8_t));
    }
    if (s_install_queue && !s_install_task)
    {
        BaseType_t ok = xTaskCreate(install_device_task, "msc_inst_w", 4096, NULL, 5, &s_install_task);
        if (ok != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create install worker task");
            s_install_task = NULL;
        }
    }

    s_mounted = false;
    s_info.state = XUSB_MSC_STATE_WAITING_DEVICE;
    s_info.capacity_bytes = 0;
    s_info.block_size = 0;

    ESP_LOGI(TAG, "USB MSC host ready; waiting for device…");
    return true;
}

void pt_usb_msc_stop(void)
{
    // Unmount if needed
    if (s_mounted && s_vfs)
    {
        (void)msc_host_vfs_unregister(s_vfs);
        s_vfs = NULL;
        s_mounted = false;
    }
    if (s_dev)
    {
        (void)msc_host_uninstall_device(s_dev);
        s_dev = NULL;
    }

    // Stop tasks
    if (s_msc_events_task)
    {
        vTaskDelete(s_msc_events_task);
        s_msc_events_task = NULL;
    }
    if (s_usb_events_task)
    {
        vTaskDelete(s_usb_events_task);
        s_usb_events_task = NULL;
    }

    // Shutdown install worker
    if (s_install_queue && s_install_task)
    {
        uint8_t sentinel = 0xFF;
        // try to notify worker to exit
        (void)xQueueSend(s_install_queue, &sentinel, pdMS_TO_TICKS(50));
        // give it a moment then delete task if still running
        vTaskDelay(pdMS_TO_TICKS(50));
        vTaskDelete(s_install_task);
        s_install_task = NULL;
    }
    if (s_install_queue)
    {
        vQueueDelete(s_install_queue);
        s_install_queue = NULL;
    }

    // Uninstall stacks
    (void)msc_host_uninstall();
    (void)usb_host_uninstall();

    s_info.state = XUSB_MSC_STATE_STOPPED;
}

bool pt_usb_msc_is_mounted(void) { return s_mounted; }

bool pt_usb_msc_get_info(pt_usb_msc_info_t *out)
{
    if (!out)
        return false;
    *out = s_info;
    return s_mounted;
}

// ---- File helpers (VFS, path rooted at XUSB_MSC_MOUNT_PATH) ----

static void make_abs(char *dst, size_t dstsz, const char *rel_or_abs)
{
    if (!rel_or_abs || rel_or_abs[0] == '\0')
    {
        snprintf(dst, dstsz, XUSB_MSC_MOUNT_PATH "/");
        return;
    }
    if (rel_or_abs[0] == '/')
        snprintf(dst, dstsz, "%s", rel_or_abs);
    else
        snprintf(dst, dstsz, XUSB_MSC_MOUNT_PATH "/%s", rel_or_abs);
}

int pt_usb_msc_list_dir(const char *path, void (*on_item)(const char *, bool, size_t, void *), void *user)
{
    if (!s_mounted)
    {
        ESP_LOGW(TAG, "List dir failed: not mounted");
        return -ENODEV;
    }

    char abs[256];
    make_abs(abs, sizeof(abs), path);
    ESP_LOGI(TAG, "Listing directory: %s", abs);

    DIR *d = opendir(abs);
    if (!d)
    {
        ESP_LOGE(TAG, "Failed to open directory: %s (errno=%d)", abs, errno);
        return -errno;
    }

    struct dirent *e;
    int count = 0;
    while ((e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", abs, e->d_name);

        struct stat st;
        bool ok = (stat(child, &st) == 0);
        bool is_dir = ok && S_ISDIR(st.st_mode);
        size_t size = ok ? (size_t)st.st_size : 0;

        ESP_LOGI(TAG, "[%s] %s size=%zu", is_dir ? "DIR" : "FILE", e->d_name, size);

        if (on_item)
            on_item(e->d_name, is_dir, size, user);

        count++;
    }
    closedir(d);
    ESP_LOGI(TAG, "Listed %d items in %s", count, abs);
    return 0;
}

/* Helper: collect names into a dynamically allocated array via the existing callback API */
static void collect_names_cb(const char *name, bool is_dir, size_t size, void *user)
{
    char ***parr = (char ***)user; // we pass pointer to (char***) where first element is the array pointer, second is a size_t pointer carried separately
    // Not used; actual collector uses closure struct instead. This stub kept for API symmetry.
    (void)name;
    (void)is_dir;
    (void)size;
    (void)parr;
}

int pt_usb_msc_list_dir_array(const char *path, char ***out_names, size_t *out_count)
{
    if (!out_names || !out_count)
        return -EINVAL;
    *out_names = NULL;
    *out_count = 0;

    /* We'll implement by calling pt_usb_msc_list_dir with a collector that reallocs an array */
    struct collector
    {
        char **arr;
        size_t count;
        size_t cap;
    } c = {NULL, 0, 0};

    void on_item(const char *name, bool is_dir, size_t size, void *user)
    {
        (void)is_dir;
        (void)size;
        struct collector *cc = (struct collector *)user;
        if (cc->count + 1 > cc->cap)
        {
            size_t newcap = cc->cap == 0 ? 8 : cc->cap * 2;
            char **tmp = realloc(cc->arr, newcap * sizeof(char *));
            if (!tmp)
                return; // allocation failure: skip item
            cc->arr = tmp;
            cc->cap = newcap;
        }
        cc->arr[cc->count] = strdup(name);
        if (!cc->arr[cc->count])
            return; // strdup fail: leave slot NULL
        cc->count++;
    }

    int r = pt_usb_msc_list_dir(path, on_item, &c);
    if (r != 0)
    {
        // free any allocated names
        for (size_t i = 0; i < c.count; ++i)
            free(c.arr[i]);
        free(c.arr);
        return r;
    }

    *out_names = c.arr;
    *out_count = c.count;
    return 0;
}

void pt_usb_msc_free_name_array(char **names, size_t count)
{
    if (!names)
        return;
    for (size_t i = 0; i < count; ++i)
    {
        free(names[i]);
    }
    free(names);
}

int pt_usb_msc_list_dir_entries(const char *path, pt_usb_msc_dir_entry_t **out_entries, size_t *out_count)
{
    if (!out_entries || !out_count)
        return -EINVAL;
    *out_entries = NULL;
    *out_count = 0;

    struct ecollector
    {
        pt_usb_msc_dir_entry_t *arr;
        size_t count;
        size_t cap;
    } ec = {NULL, 0, 0};

    void on_item(const char *name, bool is_dir, size_t size, void *user)
    {
        struct ecollector *cc = (struct ecollector *)user;
        if (cc->count + 1 > cc->cap)
        {
            size_t newcap = cc->cap == 0 ? 8 : cc->cap * 2;
            pt_usb_msc_dir_entry_t *tmp = realloc(cc->arr, newcap * sizeof(pt_usb_msc_dir_entry_t));
            if (!tmp)
                return;
            cc->arr = tmp;
            cc->cap = newcap;
        }
        pt_usb_msc_dir_entry_t *ent = &cc->arr[cc->count];
        ent->name = strdup(name);
        ent->is_dir = is_dir;
        ent->size = size;
        if (!ent->name)
            return;
        cc->count++;
    }

    int r = pt_usb_msc_list_dir(path, on_item, &ec);
    if (r != 0)
    {
        for (size_t i = 0; i < ec.count; ++i)
            free(ec.arr[i].name);
        free(ec.arr);
        return r;
    }

    *out_entries = ec.arr;
    *out_count = ec.count;
    return 0;
}

void pt_usb_msc_free_entries(pt_usb_msc_dir_entry_t *entries, size_t count)
{
    if (!entries)
        return;
    for (size_t i = 0; i < count; ++i)
    {
        free(entries[i].name);
    }
    free(entries);
}

static int ensure_parent_dirs(const char *abs_path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", abs_path);
    char *p = tmp;

    // Skip mount root "/usb/"
    if (strncmp(tmp, XUSB_MSC_MOUNT_PATH "/", strlen(XUSB_MSC_MOUNT_PATH) + 1) == 0)
    {
        p = tmp + strlen(XUSB_MSC_MOUNT_PATH) + 1;
    }

    for (; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (strlen(tmp) > 0)
                mkdir(tmp, 0777);
            *p = '/';
        }
    }
    return 0;
}

int pt_usb_msc_mkdir_p(const char *rel_path)
{
    if (!s_mounted)
        return -ENODEV;
    char abs[512];
    make_abs(abs, sizeof(abs), rel_path);
    return ensure_parent_dirs(abs);
}

int pt_usb_msc_write_file(const char *rel_path, const void *data, size_t len, bool append)
{
    if (!s_mounted)
        return -ENODEV;
    char abs[512];
    make_abs(abs, sizeof(abs), rel_path);

    ensure_parent_dirs(abs);

    FILE *f = fopen(abs, append ? "ab" : "wb");
    if (!f)
        return -errno;

    size_t w = fwrite(data, 1, len, f);
    int e = (w == len) ? 0 : -EIO;
    fclose(f);
    return e;
}

int pt_usb_msc_read_file(const char *rel_path, void *buf, size_t buf_size, size_t *out_len)
{
    if (!s_mounted)
        return -ENODEV;
    char abs[512];
    make_abs(abs, sizeof(abs), rel_path);

    FILE *f = fopen(abs, "rb");
    if (!f)
        return -errno;

    size_t r = fread(buf, 1, buf_size, f);
    if (out_len)
        *out_len = r;
    fclose(f);
    return 0;
}

int pt_usb_msc_remove(const char *rel_path)
{
    if (!s_mounted)
        return -ENODEV;
    char abs[512];
    make_abs(abs, sizeof(abs), rel_path);
    return (unlink(abs) == 0) ? 0 : -errno;
}

// ========== Internal: mount/unmount + callbacks ==========

static void mount_vfs(msc_host_device_handle_t dev)
{
    if (s_mounted)
        return;

    /* esp_vfs_fat_mount_config_t is used by the current msc_host_vfs API */
    esp_vfs_fat_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 0,
    };
    esp_err_t mr = msc_host_vfs_register(dev, XUSB_MSC_MOUNT_PATH, &mnt, &s_vfs);
    if (mr != ESP_OK)
    {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(mr));
        s_vfs = NULL;
        s_mounted = false;
        s_info.state = XUSB_MSC_STATE_WAITING_DEVICE;
        return;
    }
    s_mounted = true;
    s_info.state = XUSB_MSC_STATE_MOUNTED;

    msc_host_device_info_t info;
    if (msc_host_get_device_info(dev, &info) == ESP_OK)
    {
        /* new API exposes sector_count and sector_size */
        s_info.capacity_bytes = (uint64_t)info.sector_count * (uint64_t)info.sector_size;
        s_info.block_size = info.sector_size;
        ESP_LOGI(TAG, "Mounted at %s (%.2f GB, block %" PRIu32 ")",
                 XUSB_MSC_MOUNT_PATH,
                 (double)s_info.capacity_bytes / (1024.0 * 1024.0 * 1024.0),
                 info.sector_size);
    }
    else
    {
        s_info.capacity_bytes = 0;
        s_info.block_size = 0;
        ESP_LOGI(TAG, "Mounted at %s", XUSB_MSC_MOUNT_PATH);
    }
}

static void unmount_vfs(void)
{
    if (!s_mounted)
        return;
    if (s_vfs)
    {
        (void)msc_host_vfs_unregister(s_vfs);
        s_vfs = NULL;
    }
    s_mounted = false;
    s_info.state = XUSB_MSC_STATE_WAITING_DEVICE;
    ESP_LOGW(TAG, "Unmounted %s", XUSB_MSC_MOUNT_PATH);
}

static void msc_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    switch (event->event)
    {
    case MSC_DEVICE_CONNECTED:
    {
        ESP_LOGI(TAG, "MSC device connected: addr=%u",
                 event->device.address);

        /* install device by address (new API) */
        // Don't call potentially blocking install from the driver's callback.
        // Enqueue the device address for the install worker to process.
        if (s_install_queue)
        {
            uint8_t addr = (uint8_t)event->device.address;
            BaseType_t sent = xQueueSendToBack(s_install_queue, &addr, 0);
            if (sent != pdTRUE)
            {
                ESP_LOGW(TAG, "Install queue full; dropping device addr %u", addr);
            }
        }
        else
        {
            ESP_LOGW(TAG, "No install queue available; cannot process device %u", event->device.address);
        }
        break;
    }
    case MSC_DEVICE_DISCONNECTED:
    {
        ESP_LOGW(TAG, "MSC device disconnected");
        unmount_vfs();
        if (s_dev)
        {
            (void)msc_host_uninstall_device(s_dev);
            s_dev = NULL;
        }
        break;
    }
    default:
        break;
    }
}

// ========== Event pumping tasks ==========

static void usb_host_events_task(void *arg)
{
    (void)arg;
    while (1)
    {
        uint32_t flags = 0;
        esp_err_t r = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (r != ESP_OK)
            continue;

        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            usb_host_device_free_all();
        }
        // If ALL_FREE is set, library has no internal allocations; keep task alive.
    }
}

static void msc_events_task(void *arg)
{
    (void)arg;
    while (msc_host_handle_events(portMAX_DELAY) == ESP_OK)
    {
        // drain MSC driver events
    }
    vTaskDelete(NULL);
}

// Task spawned to perform msc_host_install_device off the driver's callback context.
static void install_device_task(void *arg)
{
    (void)arg;
    uint8_t addr = 0;
    for (;;)
    {
        if (!s_install_queue)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (xQueueReceive(s_install_queue, &addr, portMAX_DELAY) != pdTRUE)
            continue;

        // sentinel to exit
        if (addr == 0xFF)
            break;

        ESP_LOGI(TAG, "install_worker: processing device addr=%u", addr);

        // Give USB host a short moment to finish enumeration before first attempt
        vTaskDelay(pdMS_TO_TICKS(1000));

        esp_err_t r = ESP_FAIL;
        int attempt = 0;
        for (attempt = 0; attempt < XUSB_INSTALL_MAX_RETRIES; ++attempt)
        {
            ESP_LOGI(TAG, "attempt %d: calling msc_host_install_device for addr=%u", attempt + 1, addr);
            r = msc_host_install_device((uint8_t)addr, &s_dev);
            if (r == ESP_OK)
                break;

            ESP_LOGW(TAG, "msc_host_install_device attempt %d failed: %s", attempt + 1, esp_err_to_name(r));
            xusb_delay_ms(XUSB_INSTALL_RETRY_DELAY_MS);
        }

        if (r == ESP_OK)
        {
            ESP_LOGI(TAG, "msc_host_install_device OK (attempt %d)", attempt + 1);
            mount_vfs(s_dev);
            if (s_mounted)
            {
                ESP_LOGI(TAG, "Mounted at %s", XUSB_MSC_MOUNT_PATH);
                // small delay to let FS settle
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            else
            {
                ESP_LOGW(TAG, "Device installed but mount_vfs reported failure");
            }
        }
        else
        {
            ESP_LOGE(TAG, "msc_host_install_device failed after %d attempts: %s", XUSB_INSTALL_MAX_RETRIES, esp_err_to_name(r));
        }
    }

    ESP_LOGI(TAG, "install_worker: exiting");
    vTaskDelete(NULL);
}

// Self-test: list the mount root and log up to 10 items, then total found.
static void self_test_cb(const char *name, bool is_dir, size_t size, void *user)
{
    int *counter = (int *)user;
    if (!counter)
        return;
    if (*counter < 10)
    {
        ESP_LOGI(TAG, "  %s %s %zu", is_dir ? "DIR" : "FILE", name, size);
    }
    (*counter)++;
}

static void self_test_list_root(void)
{
    int count = 0;
    ESP_LOGI(TAG, "Self-test: listing %s root...", XUSB_MSC_MOUNT_PATH);
    int r = pt_usb_msc_list_dir(XUSB_MSC_MOUNT_PATH, self_test_cb, &count);
    if (r != 0)
    {
        ESP_LOGE(TAG, "Self-test: listing failed: %d", r);
        return;
    }
    ESP_LOGI(TAG, "Self-test: found %d items under %s", count, XUSB_MSC_MOUNT_PATH);
}
