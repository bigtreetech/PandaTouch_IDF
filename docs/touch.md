# PandaTouch Touch Driver

- The driver manages an I2C master bus and a device handle for the GT911 touch controller.
- It provides helpers to initialize the device, check I2C readiness, and read touch events (up to `PT_GT911_MAX_POINTS` points).
- Coordinate packing/unpacking and basic bounds-clamping are performed by the driver; the higher-level LVGL glue maps those coordinates into display space.

## Public API

- `esp_err_t pt_touch_begin(void)`

  - Initialize the I2C bus, probe the touch controller (tries addresses 0x14 and 0x5D), perform reset/address selection if needed, and add the device to the I2C master bus.
  - Returns `ESP_OK` on success or an `esp_err_t` error code such as `ESP_ERR_NOT_FOUND` when the controller cannot be located.

- `bool pt_touch_i2c_ready(void)`

  - Quick readiness check: reads the GT911 STATUS register and returns `true` when the controller indicates data ready.

- `bool pt_touch_get_touch(pt_touch_event_t *ev)`
  - Fill `ev` with the current touch snapshot. Returns `true` and sets `ev->number` > 0 when data was read successfully.
- On success, reads STATUS, unpacks track ID, X/Y, and size for each point, clamps coordinates, and clears STATUS to acknowledge.
  - Returns `false` if there is no new data or on read errors.

Note: the higher-level LVGL glue automatically registers an LVGL input device using this driver when you call `pt_display_init()`; that function invokes `pt_lvgl_touch_init(pt_disp, 800, 480)` during startup. You only need to call the low-level `pt_touch_*` APIs directly if you are building a custom input path or using a different UI stack.

### Data types

- `pt_touch_point_t` — single point: `{ uint8_t track_id; uint16_t x; uint16_t y; uint16_t size; }`
- `pt_touch_event_t` — container: `{ uint8_t number; pt_touch_point_t point[PT_GT911_MAX_POINTS]; }`

## Examples

1. Initialize the touch controller at startup

```c
if (pt_touch_begin() != ESP_OK) {
		ESP_LOGE("app", "touch init failed");
		return;
}
```

2. Poll for touches (simplest loop)

```c
pt_touch_event_t ev;
if (pt_touch_get_touch(&ev)) {
		for (uint8_t i = 0; i < ev.number; ++i) {
				printf("touch %u: id=%u x=%u y=%u size=%u\n", i, ev.point[i].track_id, ev.point[i].x, ev.point[i].y, ev.point[i].size);
		}
}
```
