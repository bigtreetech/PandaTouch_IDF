#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>

// --- LVGL v9 stdio FS driver implementation ---
// LVGL v9 API: open returns a handle (void*), seek has whence, directory ops use handles
static void *lvgl_stdio_open(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    const char *m = (mode == LV_FS_MODE_WR) ? "wb" : (mode == LV_FS_MODE_RD) ? "rb"
                                                                             : "rb+";
    char tmp[512];
    const char *use_path = path;
    if (path && path[0] != '/')
    {
        /* Prepend leading '/' so LVGL's resolved path becomes an absolute POSIX path */
        snprintf(tmp, sizeof(tmp), "/%s", path);
        use_path = tmp;
    }
    FILE *fp = fopen(use_path, m);
    return fp; /* returned as the file handle */
}

static lv_fs_res_t lvgl_stdio_close(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    FILE *fp = (FILE *)file_p;
    if (fp)
        fclose(fp);
    return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_stdio_read(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    (void)drv;
    FILE *fp = (FILE *)file_p;
    size_t r = fread(buf, 1, btr, fp);
    if (br)
        *br = (uint32_t)r;
    return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_stdio_write(lv_fs_drv_t *drv, void *file_p, const void *buf, uint32_t btw, uint32_t *bw)
{
    (void)drv;
    FILE *fp = (FILE *)file_p;
    size_t w = fwrite(buf, 1, btw, fp);
    if (bw)
        *bw = (uint32_t)w;
    return (w == btw) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t lvgl_stdio_seek(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    (void)drv;
    FILE *fp = (FILE *)file_p;
    int w = SEEK_SET;
    if (whence == LV_FS_SEEK_SET)
        w = SEEK_SET;
    else if (whence == LV_FS_SEEK_CUR)
        w = SEEK_CUR;
    else if (whence == LV_FS_SEEK_END)
        w = SEEK_END;
    return (fseek(fp, (long)pos, w) == 0) ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t lvgl_stdio_tell(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    FILE *fp = (FILE *)file_p;
    long off = ftell(fp);
    if (off < 0)
        return LV_FS_RES_FS_ERR;
    if (pos_p)
        *pos_p = (uint32_t)off;
    return LV_FS_RES_OK;
}

static void *lvgl_stdio_dir_open(lv_fs_drv_t *drv, const char *path)
{
    (void)drv;
    char tmp[512];
    const char *use_path = path;
    if (path && path[0] != '/')
    {
        snprintf(tmp, sizeof(tmp), "/%s", path);
        use_path = tmp;
    }
    DIR *d = opendir(use_path);
    return d;
}

static lv_fs_res_t lvgl_stdio_dir_read(lv_fs_drv_t *drv, void *dir_p, char *fn, uint32_t fn_size)
{
    (void)drv;
    DIR *d = (DIR *)dir_p;
    struct dirent *ent = readdir(d);
    if (!ent)
        return LV_FS_RES_FS_ERR;
    if (fn && fn_size)
        strncpy(fn, ent->d_name, fn_size - 1), fn[fn_size - 1] = '\0';
    return LV_FS_RES_OK;
}

static lv_fs_res_t lvgl_stdio_dir_close(lv_fs_drv_t *drv, void *dir_p)
{
    (void)drv;
    DIR *d = (DIR *)dir_p;
    if (d)
        closedir(d);
    return LV_FS_RES_OK;
}

void pt_lvgl_stdio_fs_init(void)
{
    static bool inited = false;
    if (inited)
        return;
    inited = true;

    static lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = '/'; /* use '/' to support POSIX absolute paths like "/usb/..." */
    /* assign callbacks directly matching lv_fs_drv_t fields */
    drv.open_cb = lvgl_stdio_open;
    drv.close_cb = lvgl_stdio_close;
    drv.read_cb = lvgl_stdio_read;
    drv.write_cb = lvgl_stdio_write;
    drv.seek_cb = lvgl_stdio_seek;
    drv.tell_cb = lvgl_stdio_tell;
    /* LVGL v9 does not have size_cb */
    drv.dir_open_cb = lvgl_stdio_dir_open;
    drv.dir_read_cb = lvgl_stdio_dir_read;
    drv.dir_close_cb = lvgl_stdio_dir_close;
    lv_fs_drv_register(&drv);
}