// pt_usb.c — ESP-IDF 5.1 ready

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
#define TAG "pt_usb"

#ifndef PT_USB_HOST_TASK_STACK
#define PT_USB_HOST_TASK_STACK 4096
#endif
#ifndef PT_USB_EVENTS_TASK_STACK
#define PT_USB_EVENTS_TASK_STACK 4096
#endif

// install retry behaviour
#ifndef PT_USB_INSTALL_MAX_RETRIES
#define PT_USB_INSTALL_MAX_RETRIES 5
#endif
#ifndef PT_USB_INSTALL_RETRY_DELAY_MS
#define PT_USB_INSTALL_RETRY_DELAY_MS 500
#endif

// -------- State --------
static TaskHandle_t s_usb_events_task = NULL;
static TaskHandle_t s_pt_usb_msc_events_task = NULL;
static TaskHandle_t s_install_task = NULL;
static QueueHandle_t s_install_queue = NULL;

static msc_host_device_handle_t s_dev = NULL;
static msc_host_vfs_handle_t s_vfs = NULL;

static volatile bool s_mounted = false;
static pt_usb_info_t s_info = {.state = PT_USB_STATE_STOPPED};

// User-registered callbacks
static PandaTouchEventCallback s_on_mount_cb = NULL;
static PandaTouchEventCallback s_on_unmount_cb = NULL;

// Forward decls
static void pt_usb_host_events_task(void *arg);
static void pt_usb_msc_events_task(void *arg);
static void pt_usb_msc_cb(const msc_host_event_t *event, void *arg);
static void pt_usb_mount_vfs(msc_host_device_handle_t dev);
static void pt_usb_unmount_vfs(void);
static void pt_usb_install_device_task(void *arg);
static void pt_usb_make_abs(char *dst, size_t dstsz, const char *rel_or_abs);
static int pt_usb_ensure_parent_dirs(const char *abs_path);

// ========== Public API ==========

bool pt_usb_start(void)
{
    if (s_info.state != PT_USB_STATE_STOPPED)
    {
        return true;
    }

    // 1) Start USB Host library
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));

    // 2) Install MSC Host driver (we pump events in our own task)
    const msc_host_driver_config_t msc_cfg = {
        .create_backround_task = false,
        .callback = pt_usb_msc_cb,
        .callback_arg = NULL,
        .task_priority = 0,
        .stack_size = 0,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_cfg));

    // 3) Event loops
    xTaskCreate(pt_usb_host_events_task, "usb_host_ev", PT_USB_HOST_TASK_STACK, NULL, 5, &s_usb_events_task);
    xTaskCreate(pt_usb_msc_events_task, "msc_host_ev", PT_USB_EVENTS_TASK_STACK, NULL, 5, &s_pt_usb_msc_events_task);

    // increase USB/MSC logging for diagnostics
    esp_log_level_set("USB", ESP_LOG_DEBUG);
    esp_log_level_set("USB_HOST", ESP_LOG_DEBUG);

    // install worker queue and task (small, bounded)
    if (!s_install_queue)
    {
        s_install_queue = xQueueCreate(4, sizeof(uint8_t));
    }
    if (s_install_queue && !s_install_task)
    {
        BaseType_t ok = xTaskCreate(pt_usb_install_device_task, "msc_inst_w", 4096, NULL, 5, &s_install_task);
        if (ok != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to create install worker task");
            s_install_task = NULL;
        }
    }

    s_mounted = false;
    s_info.state = PT_USB_STATE_WAITING_DEVICE;
    s_info.capacity_bytes = 0;
    s_info.block_size = 0;

    ESP_LOGI(TAG, "USB MSC host ready; waiting for device…");
    return true;
}

void pt_usb_on_mount(PandaTouchEventCallback cb)
{
    s_on_mount_cb = cb;
    /* If already mounted at registration time, dispatch immediately so late-joining
       clients get the mount notification. Call synchronously in the caller's context. */
    if (s_on_mount_cb && s_mounted)
    {
        s_on_mount_cb();
    }
}

void pt_usb_on_unmount(PandaTouchEventCallback cb)
{
    s_on_unmount_cb = cb;
    /* If already unmounted at registration time, dispatch immediately. */
    if (s_on_unmount_cb && !s_mounted)
    {
        s_on_unmount_cb();
    }
}

void pt_usb_stop(void)
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
    if (s_pt_usb_msc_events_task)
    {
        vTaskDelete(s_pt_usb_msc_events_task);
        s_pt_usb_msc_events_task = NULL;
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

    s_info.state = PT_USB_STATE_STOPPED;
}

bool pt_usb_is_mounted(void) { return s_mounted; }

bool pt_usb_get_info(pt_usb_info_t *out)
{
    if (!out)
    {
        return false;
    }
    *out = s_info;
    return s_mounted;
}

pt_usb_dir_list_t *pt_usb_list_dir(const char *path, int *out_err)
{
    if (out_err)
    {
        *out_err = 0;
    }

    if (!s_mounted)
    {
        if (out_err)
        {
            *out_err = -ENODEV;
        }
        return NULL;
    }

    char abs[256];
    pt_usb_make_abs(abs, sizeof(abs), path);

    DIR *d = opendir(abs);
    if (!d)
    {
        if (out_err)
        {
            *out_err = -errno;
        }
        return NULL;
    }

    struct pt_collector
    {
        pt_usb_dir_entry_t *arr;
        size_t count;
        size_t cap;
        int err;
    } c = {NULL, 0, 0, 0};

    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        {
            continue;
        }

        if (c.count + 1 > c.cap)
        {
            size_t newcap = c.cap == 0 ? 8 : c.cap * 2;
            pt_usb_dir_entry_t *tmp = realloc(c.arr, newcap * sizeof(pt_usb_dir_entry_t));
            if (!tmp)
            {
                c.err = -ENOMEM;
                break;
            }
            c.arr = tmp;
            c.cap = newcap;
        }

        char child[512];
        snprintf(child, sizeof(child), "%s/%s", abs, e->d_name);
        struct stat st;
        bool ok = (stat(child, &st) == 0);
        bool is_dir = ok && S_ISDIR(st.st_mode);
        size_t size = ok ? (size_t)st.st_size : 0;

        pt_usb_dir_entry_t *ent = &c.arr[c.count];
        ent->name = strdup(e->d_name);
        if (!ent->name)
        {
            c.err = -ENOMEM;
            break;
        }
        ent->path = strdup(child);
        if (!ent->path)
        {
            free(ent->name);
            c.err = -ENOMEM;
            break;
        }
        ent->is_dir = is_dir;
        ent->is_hidden = (ent->name[0] == '.');
        ent->size = size;
        c.count++;
    }

    closedir(d);

    if (c.err != 0)
    {
        for (size_t i = 0; i < c.count; ++i)
        {
            free(c.arr[i].name);
            free(c.arr[i].path);
        }
        free(c.arr);
        if (out_err)
        {
            *out_err = c.err;
        }
        return NULL;
    }

    /* shrink-to-fit */
    if (c.count < c.cap)
    {
        pt_usb_dir_entry_t *shrink = realloc(c.arr, c.count * sizeof(pt_usb_dir_entry_t));
        if (shrink)
        {
            c.arr = shrink;
        }
    }

    pt_usb_dir_list_t *list = malloc(sizeof(*list));
    if (!list)
    {
        for (size_t i = 0; i < c.count; ++i)
        {
            free(c.arr[i].name);
        }
        free(c.arr);
        if (out_err)
        {
            *out_err = -ENOMEM;
        }
        return NULL;
    }

    list->entries = c.arr;
    list->count = c.count;
    return list;
}

void pt_usb_dir_list_free(pt_usb_dir_list_t *list)
{
    if (!list)
    {
        return;
    }
    if (list->entries)
    {
        for (size_t i = 0; i < list->count; ++i)
        {
            free(list->entries[i].name);
            free(list->entries[i].path);
        }
        free(list->entries);
    }
    free(list);
}

int pt_usb_mkdir(const char *path)
{
    if (!s_mounted)
    {
        return -ENODEV;
    }
    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }
    char abs[512];
    pt_usb_make_abs(abs, sizeof(abs), path);
    return pt_usb_ensure_parent_dirs(abs);
}

/* Recursively remove directory contents then the directory itself.
 * Returns 0 on success or -errno on failure. */
static int pt_usb_rmrf(const char *abs_path)
{
    DIR *d = opendir(abs_path);
    if (!d)
    {
        return -errno;
    }

    struct dirent *e;
    int first_err = 0;
    char child[512];

    while ((e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
        {
            continue;
        }

        snprintf(child, sizeof(child), "%s/%s", abs_path, e->d_name);
        struct stat st;
        if (stat(child, &st) != 0)
        {
            if (!first_err)
            {
                first_err = -errno;
            }
            continue;
        }

        if (S_ISDIR(st.st_mode))
        {
            int r = pt_usb_rmrf(child);
            if (r != 0 && !first_err)
            {
                first_err = r;
            }
        }
        else
        {
            if (unlink(child) != 0 && !first_err)
            {
                first_err = -errno;
            }
        }
    }

    closedir(d);

    if (rmdir(abs_path) != 0)
    {
        return first_err ? first_err : -errno;
    }
    return first_err ? first_err : 0;
}

int pt_usb_rmdir(const char *path, bool recursive)
{
    if (!s_mounted)
        return -ENODEV;
    if (!path || path[0] != '/')
        return -EINVAL;
    char abs[512];
    pt_usb_make_abs(abs, sizeof(abs), path);
    /* Safety: refuse to operate outside the USB mount point. This prevents
       accidental removal of host filesystem paths should a caller provide
       an absolute path that is not under PT_USB_MOUNT_PATH. */
    size_t mount_len = strlen(PT_USB_MOUNT_PATH);
    if (!(strncmp(abs, PT_USB_MOUNT_PATH, mount_len) == 0 &&
          (abs[mount_len] == '\0' || abs[mount_len] == '/')))
    {
        return -EINVAL;
    }
    if (recursive)
    {
        return pt_usb_rmrf(abs);
    }
    return (rmdir(abs) == 0) ? 0 : -errno;
}

int pt_usb_write(const char *path, const void *data, size_t len, bool append)
{
    if (!s_mounted)
    {
        return -ENODEV;
    }
    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }
    char abs[512];
    pt_usb_make_abs(abs, sizeof(abs), path);

    pt_usb_ensure_parent_dirs(abs);

    FILE *f = fopen(abs, append ? "ab" : "wb");
    if (!f)
    {
        return -errno;
    }

    size_t w = fwrite(data, 1, len, f);
    int e = (w == len) ? 0 : -EIO;
    fclose(f);
    return e;
}

int pt_usb_read(const char *path, void *buf, size_t buf_size, size_t *out_len)
{
    if (!s_mounted)
    {
        return -ENODEV;
    }
    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }
    char abs[512];
    pt_usb_make_abs(abs, sizeof(abs), path);

    FILE *f = fopen(abs, "rb");
    if (!f)
    {
        return -errno;
    }

    size_t r = fread(buf, 1, buf_size, f);
    if (out_len)
    {
        *out_len = r;
    }
    fclose(f);
    return 0;
}

int pt_usb_remove(const char *path)
{
    if (!s_mounted)
    {
        return -ENODEV;
    }
    if (!path || path[0] != '/')
    {
        return -EINVAL;
    }
    char abs[512];
    pt_usb_make_abs(abs, sizeof(abs), path);
    return (unlink(abs) == 0) ? 0 : -errno;
}

// ---- File helpers ----

static void pt_usb_make_abs(char *dst, size_t dstsz, const char *rel_or_abs)
{
    if (!rel_or_abs || rel_or_abs[0] == '\0')
    {
        snprintf(dst, dstsz, PT_USB_MOUNT_PATH "/");
        return;
    }
    if (rel_or_abs[0] == '/')
    {
        snprintf(dst, dstsz, "%s", rel_or_abs);
    }
    else
    {
        snprintf(dst, dstsz, PT_USB_MOUNT_PATH "/%s", rel_or_abs);
    }
}

static int pt_usb_ensure_parent_dirs(const char *abs_path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", abs_path);
    char *p = tmp;

    // Skip mount root "/usb/"
    if (strncmp(tmp, PT_USB_MOUNT_PATH "/", strlen(PT_USB_MOUNT_PATH) + 1) == 0)
    {
        p = tmp + strlen(PT_USB_MOUNT_PATH) + 1;
    }

    for (; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\0';
            if (strlen(tmp) > 0)
            {
                mkdir(tmp, 0777);
            }
            *p = '/';
        }
    }
    return 0;
}

// ========== Internal: mount/unmount + callbacks ==========

static void pt_usb_mount_vfs(msc_host_device_handle_t dev)
{
    if (s_mounted)
        return;

    /* esp_vfs_fat_mount_config_t is used by the current msc_host_vfs API */
    esp_vfs_fat_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 0,
    };
    esp_err_t mr = msc_host_vfs_register(dev, PT_USB_MOUNT_PATH, &mnt, &s_vfs);
    if (mr != ESP_OK)
    {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(mr));
        s_vfs = NULL;
        s_mounted = false;
        s_info.state = PT_USB_STATE_WAITING_DEVICE;
        return;
    }
    s_mounted = true;
    s_info.state = PT_USB_STATE_MOUNTED;

    msc_host_device_info_t info;
    if (msc_host_get_device_info(dev, &info) == ESP_OK)
    {
        /* new API exposes sector_count and sector_size */
        s_info.capacity_bytes = (uint64_t)info.sector_count * (uint64_t)info.sector_size;
        s_info.block_size = info.sector_size;
        ESP_LOGI(TAG, "Mounted at %s (%.2f GB, block %" PRIu32 ")",
                 PT_USB_MOUNT_PATH,
                 (double)s_info.capacity_bytes / (1024.0 * 1024.0 * 1024.0),
                 info.sector_size);
    }
    else
    {
        s_info.capacity_bytes = 0;
        s_info.block_size = 0;
        ESP_LOGI(TAG, "Mounted at %s", PT_USB_MOUNT_PATH);
    }
    if (s_on_mount_cb)
        s_on_mount_cb();
}

static void pt_usb_unmount_vfs(void)
{
    if (!s_mounted)
        return;
    if (s_vfs)
    {
        (void)msc_host_vfs_unregister(s_vfs);
        s_vfs = NULL;
    }
    s_mounted = false;
    s_info.state = PT_USB_STATE_WAITING_DEVICE;
    ESP_LOGW(TAG, "Unmounted %s", PT_USB_MOUNT_PATH);
    if (s_on_unmount_cb)
        s_on_unmount_cb();
}

static void pt_usb_msc_cb(const msc_host_event_t *event, void *arg)
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
        pt_usb_unmount_vfs();
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

static void pt_usb_host_events_task(void *arg)
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

static void pt_usb_msc_events_task(void *arg)
{
    (void)arg;
    while (msc_host_handle_events(portMAX_DELAY) == ESP_OK)
    {
        // drain MSC driver events
    }
    vTaskDelete(NULL);
}

static void pt_usb_install_device_task(void *arg)
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
        for (attempt = 0; attempt < PT_USB_INSTALL_MAX_RETRIES; ++attempt)
        {
            ESP_LOGI(TAG, "attempt %d: calling msc_host_install_device for addr=%u", attempt + 1, addr);
            r = msc_host_install_device((uint8_t)addr, &s_dev);
            if (r == ESP_OK)
                break;

            ESP_LOGW(TAG, "msc_host_install_device attempt %d failed: %s", attempt + 1, esp_err_to_name(r));
            vTaskDelay(pdMS_TO_TICKS(PT_USB_INSTALL_RETRY_DELAY_MS));
        }

        if (r == ESP_OK)
        {
            ESP_LOGI(TAG, "msc_host_install_device OK (attempt %d)", attempt + 1);
            pt_usb_mount_vfs(s_dev);
            if (s_mounted)
            {
                ESP_LOGI(TAG, "Mounted at %s", PT_USB_MOUNT_PATH);
                // small delay to let FS settle
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            else
            {
                ESP_LOGW(TAG, "Device installed but pt_usb_mount_vfs reported failure");
            }
        }
        else
        {
            ESP_LOGE(TAG, "msc_host_install_device failed after %d attempts: %s", PT_USB_INSTALL_MAX_RETRIES, esp_err_to_name(r));
        }
    }

    ESP_LOGI(TAG, "install_worker: exiting");
    vTaskDelete(NULL);
}