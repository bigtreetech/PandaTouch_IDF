# PandaTouch_IDF 🐼

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/esp--idf-%3E=5.1-brightgreen)](https://docs.espressif.com/)
[![LVGL](https://img.shields.io/badge/LVGL-v9-orange)](https://lvgl.io/)

A compact ESP-IDF component collection for PandaTouch-style LCD + touch hardware: LVGL display glue 🖥️, GT911 touch driver ✋, and a simple USB Mass Storage (MSC) VFS wrapper 🔌 — with usage examples and safety-minded helpers.

## Table of Contents

- [Prerequisites 🚀](#prerequisites)
- [Supported software & hardware 🧰](#supported-software--hardware)
- [Features ✨](#features)
- [Documentation 📚](#documentation)
- [Installation & configuration 🛠️](#installation--configuration)
  - [LVGL memory allocator (Kconfig)](#lvgl-memory-allocator-kconfig)
  - [Kconfig options (rendering & runtime)](#kconfig-options-rendering--runtime)
  - [Create a new project 🆕](#create-a-new-project)
    - [Add via IDF Component Manager (git or local path) (recommended) 🔗](#a--add-via-idf-component-manager-git-or-local-path-recommended)
    - [Use it as a local component (components/) 📁](#b--use-it-as-a-local-component-components)
    - [Add as a Git submodule 🔀](#c--add-as-a-git-submodule)
- [Minimal project example 🧩](#minimal-project-example-mainc)
- [Usage examples 🧪](#usage-examples)
  - [Display + LVGL (minimal) 🖥️](#display--lvgl-minimal)
  - [Touch (GT911 low-level) ✋](#touch-gt911-low-level)
  - [USB-MSC (event-driven) 🔌](#usb-msc-vfs-wrapper)
- [API quick reference 📖](#api-quick-reference-)
- [Examples (where to find & how to run) 🗂️](#examples-where-to-find--how-to-run)
- [Troubleshooting ⚠️](#troubleshooting)
- [Contributing 🤝](#contributing)
- [License 📜](#license)
- [Appendix & Links 🔗](#appendix--links)

## Prerequisites

- ESP-IDF development environment.
- ESP-IDF 5.1 or newer.

## Supported software & hardware

- Target: PandaTouch (ESP32-S3)
- ESP-IDF: >= 5.1 (tested with 5.x)
- LVGL: v9 APIs used
- Touch controller: GT911 (driver included)
- Display: generic esp_lcd panel bindings

## Features

- Display initialization and LVGL binding helpers
- Thread-safe LVGL helpers: scheduler + scope-lock macro
- Backlight control API (set/get)
- GT911 touch driver with LVGL input glue
- USB MSC wrapper:
  - start/stop lifecycle
  - mount/unmount callbacks
  - read/write/mkdir/remove/rmdir (recursive option)
  - directory listing that returns owned `name` and `path` strings
- Examples demonstrating synchronous and event-driven MSC usage and LVGL interactions

## Documentation

Additional, higher-detail documentation is available in the repository. Key docs:

- [display.md](./display.md) — display & LVGL rendering strategies and usage
- [msc.md](./msc.md) — USB-MSC wrapper API, examples and ownership rules
- [docs/touch.md](./docs/touch.md) — low-level touch driver details (GT911)
- [docs/lvgl_touch.md](./docs/lvgl_touch.md) — LVGL glue and input device mapping

Open the files directly in this repo or view them in your browser via GitHub.

## Installation & configuration

### LVGL memory allocator (Kconfig)

This component ships an optional internal LVGL memory allocator enabled by the
`PT_USE_CUSTOM_INTERNAL_MALLOC` Kconfig option (which depends on `LV_USE_CUSTOM_MALLOC`).
When enabled (the default in this component), PandaTouch_IDF provides a minimal
implementation of the `lv_mem*\*`hooks that places LVGL heap allocations into
SPIRAM (when available). If you disable this option you must provide your own`lv_mem_init()` / allocator hooks for LVGL in your app.

- Default internal implementation (what the component installs when enabled):

  ```c
  void lv_mem_init(void)
  {
  }
  void *lv_malloc_core(size_t size)
  {
      return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  void *lv_realloc_core(void *p, size_t new_size)
  {
      return heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  void lv_free_core(void *p)
  {
      heap_caps_free(p);
  }
  ```

  > This implementation favors SPIRAM to reduce pressure on internal RAM. If your
  > board doesn't have SPIRAM, or you need allocations in internal memory, either
  > disable the Kconfig and provide your own `lv_mem_init()` or change the
  > allocation flags accordingly.

  Keep an eye on LVGL heap size and fragmentation for larger UIs; a custom
  allocator tuned to your board may yield better performance.

### Kconfig options (rendering & runtime)

Below are the most important Kconfig knobs that affect rendering memory and
runtime behavior. Tweak these based on whether your board has PSRAM and the
size/complexity of your UI.

- PT_LV_RENDER_METHOD / radio choice

  - Default: `PARTIAL_2_PSRAM` (ping-pong partial buffers, internal preferred).
  - See the detailed table and guidance in [docs/display.md](./docs/display.md#render-methods) (section "Render methods") and the enum definition in [include/pandatouch_display.h](./include/pandatouch_display.h).

- PT_LV_RENDER_PARTIAL_BUFFER_LINES (int, default 80)

  - Number of vertical lines per partial buffer. Higher values use more RAM
    but reduce flush frequency; lower values save memory at cost of more
    frequent flushes.

- PT_LV_RENDER_BOUNCING_BUFFER_LINES (int, default 10)

  - Scanlines used by the small bounce buffer that smooths partial flushes.

- PT_LVGL_TASK_STACK_SIZE (int, default 8 kB)
  - Stack reserved for the LVGL thread. Increase this when running larger
    displays, heavier LVGL tasks, or when using complex touch drivers.

### Create a new project

This section shows three common ways to create a fresh ESP-IDF project and include `PandaTouch_IDF`: (A) use the IDF component manager (recommended), (B) add the component locally in `components/`, or (C) add it as a Git submodule.

Prerequisites

- ESP-IDF installed and exported (`. $IDF_PATH/export.sh`).
- A working project directory (see the minimal example below).

#### (A) — Add via IDF Component Manager (git or local path) [recommended]

1. Create a new project directory (you can use the official template or a minimal layout):

   ```
   my-app/
   ├─ main/
   │   └─ main.c
   ├─ CMakeLists.txt
   └─ idf_component.yml    # project component manifest
   ```

2. In your project's `idf_component.yml` add the dependency list. Use `git:` to pull from the remote repo, or `path:` to point at a local copy during development.

   - Example `idf_component.yml` (dependency snippet):

     ```yaml
     dependencies:
     lvgl/lvgl: ~9.3.0
     espressif/usb_host_msc: ^1.1.3
     PandaTouch_IDF:
       git: https://github.com/bigtreetech/PandaTouch_IDF.git
     ```

3. Build normally with `idf.py build` and the IDF component manager will fetch remote deps or use the local path.

> The IDF component manager resolves components listed in `idf_component.yml` automatically.

#### (B) — Use it as a local component (components/)

1. Place `PandaTouch_IDF` inside your project's `components/` folder:

   ```
   my-app/
   ├─ components/
   │   └─ PandaTouch_IDF/  (copy of this repo)
   └─ main/
   ```

2. The project build system will include any component under `components/` automatically. No manifest changes required.

#### (C) — Add as a Git submodule

From your project root:

    ```bash
    git submodule add https://github.com/bigtreetech/PandaTouch_IDF.git components/PandaTouch_IDF
    git submodule update --init --recursive
    ```

Then build as usual. This keeps a tracked copy of the dependency within your repo and is useful if you want a reproducible, versioned dependency.

## Minimal project example (main.c)

- Create `main/main.c` with a tiny app that shows how to include and call display init:

  ```c
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "esp_log.h"
  #include "pandatouch_display.h"

  void app_main(void) {
      if (pt_display_init() != ESP_OK) {
          ESP_LOGE("app", "pt_display_init failed");
          while(1) vTaskDelay(pdMS_TO_TICKS(1000));
      }

      // Use pt_display_schedule_ui or PT_LVGL_SCOPE_LOCK() in your app
      while (true) {
          vTaskDelay(pdMS_TO_TICKS(10000));
      }
  }
  ```

- Project-level CMakeLists.txt (minimal)

  ```cmake
  cmake_minimum_required(VERSION 3.16)
  include($ENV{IDF_PATH}/tools/cmake/project.cmake)
  project(my_app)
  ```

## Usage examples

- Display + LVGL (minimal) 🖥️

  ```c
  if (pt_display_init() != ESP_OK) {
      ESP_LOGE("app", "display init failed");
      return;
  }

  /* Set backlight to 80% */
  if (!pt_backlight_set(80)) {
      ESP_LOGW("app", "backlight set failed");
  }

  /* Schedule UI work from another FreeRTOS task */
  pt_display_schedule_ui(my_ui_update_fn, NULL);
  ```

- Touch (LVGL glue) ✋

  ```c
  lv_indev_t *indev = pt_lvgl_touch_init(NULL, 800, 480);
  if (!indev) {
      ESP_LOGE("app", "pt_lvgl_touch_init failed");
  }
  ```

  Note: `pt_display_init()` calls `pt_lvgl_touch_init(pt_disp, 800, 480)` by default; you usually don't need to call it manually.

- USB-MSC (event-driven) 🔌

  ```c
  void on_mount(void) {
      pt_usb_dir_list_t *list = pt_usb_list_dir("/", NULL);
      if (list) {
          for (size_t i = 0; i < list->count; ++i) {
              printf("%s %s\n", list->entries[i].is_dir ? "DIR " : "FILE", list->entries[i].path);
          }
          pt_usb_dir_list_free(list);
      }
      /* Write a test file */
      const char *msg = "hello panda\n";
      pt_usb_write("/event_sample.txt", msg, strlen(msg), false);
  }

  int app_main(void) {
      pt_usb_on_mount(on_mount);
      if (!pt_usb_start()) {
          ESP_LOGE("app", "usb host start failed");
      }
      // keep running...
  }
  ```

## API quick reference 📖

A compact, scannable reference grouped by subsystem. Function | signature | short description.

### Display & LVGL

| Function                 |                                               Signature | Purpose                                                               |
| ------------------------ | ------------------------------------------------------: | --------------------------------------------------------------------- |
| `pt_display_init`        |                       `esp_err_t pt_display_init(void)` | Initialize panel, LVGL bindings, and default touch registration.      |
| `pt_backlight_set`       |               `bool pt_backlight_set(uint32_t percent)` | Set backlight (0–100). Returns `true` on success.                     |
| `pt_backlight_get`       |                       `uint32_t pt_backlight_get(void)` | Read current backlight value (0–100).                                 |
| `pt_display_schedule_ui` | `void pt_display_schedule_ui(pt_ui_fn_t fn, void *arg)` | Schedule `fn(arg)` to run on the LVGL thread (safe from other tasks). |
| `PT_LVGL_SCOPE_LOCK()`   |                                                   macro | RAII-style scope lock for safe LVGL calls from other tasks.           |

### Touch (GT911 low-level)

| Function             |                                       Signature | Purpose                                                                       |
| -------------------- | ----------------------------------------------: | ----------------------------------------------------------------------------- |
| `pt_touch_begin`     |                `esp_err_t pt_touch_begin(void)` | Initialize I2C and probe the touch controller.                                |
| `pt_touch_i2c_ready` |                 `bool pt_touch_i2c_ready(void)` | Quick check whether the touch controller reports data ready.                  |
| `pt_touch_get_touch` | `bool pt_touch_get_touch(pt_touch_event_t *ev)` | Fill `ev` with current touch snapshot; returns `true` if touch data was read. |

### LVGL glue

| Function             |                                                                Signature | Purpose                                                                                                 |
| -------------------- | -----------------------------------------------------------------------: | ------------------------------------------------------------------------------------------------------- |
| `pt_lvgl_touch_init` | `lv_indev_t *pt_lvgl_touch_init(lv_display_t *disp, int tp_w, int tp_h)` | Create and register an LVGL pointer input device mapped to the touch driver. Returns `NULL` on failure. |

### USB-MSC (VFS wrapper)

| Function               |                                                                        Signature | Purpose                                                                                                                   |
| ---------------------- | -------------------------------------------------------------------------------: | ------------------------------------------------------------------------------------------------------------------------- |
| `pt_usb_start`         |                                                        `bool pt_usb_start(void)` | Start USB host / MSC tasks and install VFS.                                                                               |
| `pt_usb_stop`          |                                                         `void pt_usb_stop(void)` | Stop host/tasks and unmount/uninstall VFS.                                                                                |
| `pt_usb_is_mounted`    |                                                   `bool pt_usb_is_mounted(void)` | Returns whether a device is currently mounted.                                                                            |
| `pt_usb_on_mount`      |                               `void pt_usb_on_mount(PandaTouchEventCallback cb)` | Register a mount callback (invoked immediately if already mounted).                                                       |
| `pt_usb_on_unmount`    |                             `void pt_usb_on_unmount(PandaTouchEventCallback cb)` | Register an unmount callback.                                                                                             |
| `pt_usb_list_dir`      |             `pt_usb_dir_list_t *pt_usb_list_dir(const char *path, int *out_err)` | List directory entries; returns allocated list (free with `pt_usb_dir_list_free`). Sets `out_err` to `-errno` on failure. |
| `pt_usb_dir_list_free` |                             `void pt_usb_dir_list_free(pt_usb_dir_list_t *list)` | Free list and owned `name`/`path` strings.                                                                                |
| `pt_usb_mkdir`         |                                             `int pt_usb_mkdir(const char *path)` | Create directory parents for `path`. Returns `0` or `-errno`.                                                             |
| `pt_usb_rmdir`         |                             `int pt_usb_rmdir(const char *path, bool recursive)` | Remove directory; set `recursive=true` to remove contents first. Returns `0` or `-errno`.                                 |
| `pt_usb_write`         |  `int pt_usb_write(const char *path, const void *data, size_t len, bool append)` | Write data (creates parents if needed). Returns `0` or `-errno`.                                                          |
| `pt_usb_read`          | `int pt_usb_read(const char *path, void *buf, size_t buf_size, size_t *out_len)` | Read up to `buf_size` bytes into `buf`, sets `out_len` when provided.                                                     |
| `pt_usb_remove`        |                                            `int pt_usb_remove(const char *path)` | Remove/unlink file.                                                                                                       |

Notes

- Error conventions: `0` = success, `-ENODEV` = not mounted, `-EINVAL` = invalid arg (e.g., non-absolute path), other negative values = `-errno` from syscalls.
- Ownership: `pt_usb_list_dir()` returns heap-allocated strings for `name` and `path` — free the list with `pt_usb_dir_list_free()`.

## Examples (where to find & how to run) 🗂️

- `examples/display_sample.c` — LVGL + scheduler + backlight demo
- `examples/msc_sample.c` — mount-callback-driven MSC demo (recommended for event-driven apps)

> Example sources shipped in `PandaTouch_IDF/examples/` are not automatically compiled by a host project. Copy files you want into your `main/` or add an example `CMakeLists.txt` that builds the desired example as an app.

How to build an example

    ```bash
    # from project root
    idf.py fullclean  # optional, helpful when switching IDF versions
    . $IDF_PATH/export.sh
    idf.py build
    idf.py -p /dev/ttyUSB0 flash monitor
    ```

If you prefer to add this component to an existing app:

- Place the repo in `components/PandaTouch_IDF` or add it to your project workspace, then include the headers and link normally.

## Troubleshooting

Common problems

- Missing ESP-IDF includes in your editor: ensure `$IDF_PATH/export.sh` has been sourced in your shell or IDE environment.

## Contributing

- Fork the repository, create a branch, and open a pull request against `master`.
- Keep changes small and focused. Add an example or test for new behavior when possible.
- Follow existing code style in `src/`. Prefer descriptive commit messages and include a short changelog entry for user-facing changes.
- Open issues for design or API changes before large refactors.

See also: [CONTRIBUTING.md](./CONTRIBUTING.md) and [CODE_OF_CONDUCT.md](./CODE_OF_CONDUCT.md)

## License 📜

This repository is provided under the MIT License (assumed). Replace or specify a different license if required.

MIT © Bigtreetech
