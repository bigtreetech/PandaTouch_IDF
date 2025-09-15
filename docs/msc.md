# PandaTouch MSC (USB Mass Storage) API

The API provides a small wrapper around the ESP-IDF MSC (USB Mass Storage) host + VFS.
It exposes mount lifecycle functions, simple file operations and a directory listing helper.

Configuration macros (compile-time):

- `PT_USB_MOUNT_PATH` (default: `/usb`) — mount point used by the VFS.
- `PT_USB_HOST_TASK_STACK`, `PT_USB_EVENTS_TASK_STACK` — task stack sizes.
- `PT_USB_INSTALL_MAX_RETRIES`, `PT_USB_INSTALL_RETRY_DELAY_MS` — install retry behaviour.

## Types

- `pt_usb_state_t` — enum describing USB state (`PT_USB_STATE_STOPPED`, `PT_USB_STATE_WAITING_DEVICE`, `PT_USB_STATE_MOUNTED`).

- `pt_usb_info_t` — snapshot of device info:

  - `state` — `pt_usb_state_t`
  - `capacity_bytes` — total capacity in bytes (0 if unknown)
  - `block_size` — device block size in bytes (0 if unknown)

- `pt_usb_dir_entry_t` — one directory entry returned by `pt_usb_list_dir`:

  - `char *name` — filename only (caller must free with `pt_usb_dir_list_free`).
  - `char *path` — full absolute path (duplicate of internal path; caller must free).
  - `bool is_dir` — true if entry is a directory.
  - `bool is_hidden` — true if filename starts with `.`.
  - `size_t size` — file size in bytes (0 for directories or unknown).

- `pt_usb_dir_list_t` — owns an array of `pt_usb_dir_entry_t` and its `count`. Free with `pt_usb_dir_list_free()`.

## Initialization and lifecycle

- `bool pt_usb_start(void)` — starts host/driver and event tasks. Returns true on success.
- `void pt_usb_stop(void)` — stops tasks, unmounts and uninstalls drivers.
- `bool pt_usb_is_mounted(void)` — returns current mounted state.
- `bool pt_usb_get_info(pt_usb_info_t *out)` — fills `out` with last known info and returns `s_mounted`.

## Callbacks

- `void pt_usb_on_mount(PandaTouchEventCallback cb)` — register mount callback. If already mounted the callback is invoked synchronously at registration.
- `void pt_usb_on_unmount(PandaTouchEventCallback cb)` — register unmount callback. If already unmounted the callback is invoked synchronously at registration.

## Directory listing

- `pt_usb_dir_list_t *pt_usb_list_dir(const char *path, int *out_err)`

  - `path` may be absolute or relative (legacy `make_abs` support); the implementation historically accepted relative paths and prefixed with `PT_USB_MOUNT_PATH`.
  - On success returns a newly-allocated `pt_usb_dir_list_t` (caller must call `pt_usb_dir_list_free()` to free `entries`, `name`, `path`, and the list object).
  - On failure returns `NULL` and, if `out_err` provided, sets it to a negative errno-like code (for example `-ENODEV` if not mounted).

- `void pt_usb_dir_list_free(pt_usb_dir_list_t *list)` — frees `list` and all owned memory.

## File and directory helpers

All public file/directory API functions require the `path` argument to be an absolute path (leading `/`). If a non-absolute path is provided they return `-EINVAL`. They return `-ENODEV` when the device is not mounted, and `-errno` on underlying syscall failures.

- `int pt_usb_mkdir(const char *path)`

  - Ensures parent directories exist for the provided absolute `path` by creating them. Returns 0 on success.

- `int pt_usb_rmdir(const char *path, bool recursive)`

  - If `recursive` is `false` calls `rmdir(path)`.
  - If `recursive` is `true` removes directory contents recursively then removes the directory itself. Returns 0 on success.

- `int pt_usb_write(const char *path, const void *data, size_t len, bool append)`

  - Writes `len` bytes to `path`, creating parent directories first. Use `append=true` to append. Returns 0 or `-errno`.

- `int pt_usb_read(const char *path, void *buf, size_t buf_size, size_t *out_len)`

  - Reads up to `buf_size` bytes into `buf`. Writes number of bytes read to `out_len` if provided. Returns 0 or `-errno`.

- `int pt_usb_remove(const char *path)`
  - Unlinks the file at `path`. Returns 0 or `-errno`.

## Error conventions

- `0` — success
- `-ENODEV` — device not mounted
- `-EINVAL` — invalid argument (e.g. path not absolute)
- other negative values — `-errno` from the underlying POSIX call

## Ownership and memory

- `pt_usb_list_dir()` returns ownership of the returned `pt_usb_dir_list_t *` to the caller. Call `pt_usb_dir_list_free()` to free the list and each `entry`'s `name` and `path`.
- Strings returned (`name`, `path`) are heap-allocated with `strdup`.

## Example usage

```c
pt_usb_dir_list_t *list = pt_usb_list_dir("/", NULL);
if (list) {
    for (size_t i = 0; i < list->count; ++i) {
        printf("%s -> %s\n", list->entries[i].name, list->entries[i].path);
    }
    pt_usb_dir_list_free(list);
}

int err = pt_usb_write("/mydir/file.txt", data, data_len, false);
if (err != 0) {
    // handle error (err is negative errno)
}

// remove directory recursively
pt_usb_rmdir("/mydir", true);
```

## Safety notes and suggestions

- `pt_usb_rmdir(..., true)` will recursively delete files under the provided path. Consider adding checks to ensure the path is inside `PT_USB_MOUNT_PATH` before calling recursive removal.
- Current implementation uses fixed-size buffers for path composition; if you need to support very long paths consider switching to dynamic allocation.

## Location

The public API is declared in `include/pandatouch_msc.h` and implemented in `src/pandatouch_msc.c`.

---

If you'd like, I can:

- add a short unit test exercising the create/read/write/remove flow, or
- expand the Safety section with a built-in guard preventing removal outside `PT_USB_MOUNT_PATH`.

## More examples

Below are more complete, copy-paste ready snippets showing common workflows.

1. Mount + callback example

```c
void on_mount(void) {
  printf("USB mounted at %s\n", PT_USB_MOUNT_PATH);
}

void setup(void) {
  pt_usb_on_mount(on_mount);
  pt_usb_start();
}
```

2. List root and print types

```c
int err = 0;
pt_usb_dir_list_t *list = pt_usb_list_dir("/", &err);
if (!list) {
  fprintf(stderr, "list failed: %d\n", err);
} else {
  for (size_t i = 0; i < list->count; ++i) {
    pt_usb_dir_entry_t *e = &list->entries[i];
    printf("%s (%s) size=%zu path=%s\n", e->name, e->is_dir ? "DIR" : "FILE", e->size, e->path);
  }
  pt_usb_dir_list_free(list);
}
```

3. Write and read back a small blob

```c
const char *path = "/logs/session.txt";
const char *txt = "hello panda\n";
int werr = pt_usb_write(path, txt, strlen(txt), true);
if (werr != 0) {
  fprintf(stderr, "write failed: %d\n", werr);
}

char buf[256];
size_t got = 0;
int rerr = pt_usb_read(path, buf, sizeof(buf)-1, &got);
if (rerr == 0) {
  buf[got] = '\0';
  printf("read: %s", buf);
} else {
  fprintf(stderr, "read failed: %d\n", rerr);
}
```

4. Create nested directories and safely remove recursively

```c
// Create parents for /backup/2025/09
int m = pt_usb_mkdir("/backup/2025/09");
if (m != 0) {
  fprintf(stderr, "mkdir failed: %d\n", m);
}

// Safety: ensure path is inside the mount root before removing recursively
const char *target = "/backup/2025";
if (strncmp(target, PT_USB_MOUNT_PATH, strlen(PT_USB_MOUNT_PATH)) == 0 || target[0] == '/') {
  int rr = pt_usb_rmdir(target, true);
  if (rr != 0) {
    fprintf(stderr, "rmdir recursive failed: %d\n", rr);
  }
} else {
  fprintf(stderr, "refused to remove outside mount: %s\n", target);
}
```

5. Error handling patterns

```c
int err = pt_usb_write("/foo/bar", data, n, false);
if (err == -ENODEV) {
  // not mounted
} else if (err == -EINVAL) {
  // path was not absolute or invalid
} else if (err != 0) {
  // other errno: -err contains standard errno
}
```

If you want I can also add an example that uses dynamic buffers for very large reads, or demonstrate how to wrap these APIs into a higher-level safe utility that forbids deleting outside `PT_USB_MOUNT_PATH`.
