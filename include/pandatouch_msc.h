// pandatouch_msc.h — minimal stub
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PT_USB_MOUNT_PATH
#define PT_USB_MOUNT_PATH "/usb"
#endif

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

typedef enum
{
    PT_USB_STATE_STOPPED = 0,
    PT_USB_STATE_WAITING_DEVICE,
    PT_USB_STATE_MOUNTED,
} pt_usb_state_t;

typedef struct
{
    pt_usb_state_t state;
    unsigned long long capacity_bytes;
    unsigned int block_size;
} pt_usb_info_t;

typedef struct
{
    char *name;     /* filename only, not full path */
    char *path;     /* full absolute path */
    bool is_dir;    /* true if entry is a directory */
    bool is_hidden; /* true if filename is hidden (starts with '.') */
    size_t size;    /* file size in bytes (0 for directories or unknown) */
} pt_usb_dir_entry_t;

/* Object returned by new list API: owns the entries array; free with pt_usb_dir_list_free(). */
typedef struct pt_usb_dir_list
{
    pt_usb_dir_entry_t *entries;
    size_t count;
} pt_usb_dir_list_t;

#ifdef __cplusplus
extern "C"
{
#endif

    typedef void (*PandaTouchEventCallback)(void);

    bool pt_usb_start(void);
    void pt_usb_stop(void);
    bool pt_usb_is_mounted(void);
    bool pt_usb_get_info(pt_usb_info_t *out);

    pt_usb_dir_list_t *pt_usb_list_dir(const char *path, int *out_err);
    void pt_usb_dir_list_free(pt_usb_dir_list_t *list);
    int pt_usb_mkdir(const char *path);
    int pt_usb_rmdir(const char *path, bool recursive);
    int pt_usb_write(const char *path, const void *data, size_t len, bool append);
    int pt_usb_read(const char *path, void *buf, size_t buf_size, size_t *out_len);
    int pt_usb_remove(const char *path);
    void pt_usb_on_mount(PandaTouchEventCallback cb);
    void pt_usb_on_unmount(PandaTouchEventCallback cb);

#ifdef __cplusplus
}
#endif
