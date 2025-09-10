// pandatouch_msc.h — minimal stub
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef XUSB_MSC_MOUNT_PATH
#define XUSB_MSC_MOUNT_PATH "/usb"
#endif

typedef enum
{
    XUSB_MSC_STATE_STOPPED = 0,
    XUSB_MSC_STATE_WAITING_DEVICE,
    XUSB_MSC_STATE_MOUNTED,
} pt_usb_msc_state_t;

typedef struct
{
    pt_usb_msc_state_t state;
    unsigned long long capacity_bytes;
    unsigned int block_size;
} pt_usb_msc_info_t;

typedef struct
{
    char *name;  /* filename only, not full path */
    bool is_dir; /* true if entry is a directory */
    size_t size; /* file size in bytes (0 for directories or unknown) */
} pt_usb_msc_dir_entry_t;

#ifdef __cplusplus
extern "C"
{
#endif

    bool pt_usb_msc_start(void);
    void pt_usb_msc_stop(void);
    bool pt_usb_msc_is_mounted(void);
    bool pt_usb_msc_get_info(pt_usb_msc_info_t *out);

    int pt_usb_msc_list_dir(const char *path, void (*on_item)(const char *name, bool is_dir, size_t size, void *user), void *user);
    /* Return array of null-terminated strings (names). Caller must free with pt_usb_msc_free_name_array. */
    int pt_usb_msc_list_dir_array(const char *path, char ***out_names, size_t *out_count);
    void pt_usb_msc_free_name_array(char **names, size_t count);

    /* Return array of detailed entries. Caller must free with pt_usb_msc_free_entries. */
    int pt_usb_msc_list_dir_entries(const char *path, pt_usb_msc_dir_entry_t **out_entries, size_t *out_count);
    void pt_usb_msc_free_entries(pt_usb_msc_dir_entry_t *entries, size_t count);
    int pt_usb_msc_mkdir_p(const char *rel_path);
    int pt_usb_msc_write_file(const char *rel_path, const void *data, size_t len, bool append);
    int pt_usb_msc_read_file(const char *rel_path, void *buf, size_t buf_size, size_t *out_len);
    int pt_usb_msc_remove(const char *rel_path);

#ifdef __cplusplus
}
#endif
