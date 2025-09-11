#pragma once

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Initialize an LVGL filesystem driver that uses standard C stdio/DIR
     * functions to access POSIX-style paths (supports absolute paths like "/usb/...").
     *
     * This function is idempotent (safe to call multiple times).
     */
    void pt_lvgl_stdio_fs_init(void);

#ifdef __cplusplus
}
#endif
