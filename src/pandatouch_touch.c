#include "pandatouch_touch.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

/* --------- Logging --------- */
static const char *TAG = "PandaTouch::Touch";

/* --------- I2C handles --------- */
static i2c_master_bus_handle_t pt_i2c_bus = NULL;
static i2c_master_dev_handle_t pt_i2c_dev = NULL;

/* --------- Helpers --------- */
static inline void pt_touch_cfg_out(int gpio, int level)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io);
    gpio_set_level(gpio, level);
}

static inline void pt_touch_cfg_in_pu(int gpio)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io);
}

static inline bool pt_touch_probe_addr(i2c_master_bus_handle_t bus, uint8_t addr, int timeout_ms)
{
    return i2c_master_probe(bus, addr, timeout_ms) == ESP_OK;
}

/* INT level during reset chooses addr: INT=0 -> 0x14, INT=1 -> 0x5D */
static void pt_touch_addr_select(bool choose_5D)
{
#if (PT_GT911_RST_GPIO >= 0) && (PT_GT911_INT_GPIO >= 0)
    pt_touch_cfg_out(PT_GT911_INT_GPIO, choose_5D ? 1 : 0);
    pt_touch_cfg_out(PT_GT911_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PT_GT911_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    pt_touch_cfg_in_pu(PT_GT911_INT_GPIO);
    vTaskDelay(pdMS_TO_TICKS(10));
#else
    (void)choose_5D;
#endif
}

/* --------- Low-level I2C (16-bit regs) --------- */
static esp_err_t pt_touch_i2c_read(uint16_t reg, void *buf, size_t len)
{
    uint8_t reg_be[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};

    // Try repeated-start
    esp_err_t err = i2c_master_transmit_receive(pt_i2c_dev, reg_be, 2, buf, len, -1);
    if (err == ESP_OK)
        return ESP_OK;

    // Fallback: STOP between write & read
    ESP_RETURN_ON_ERROR(i2c_master_transmit(pt_i2c_dev, reg_be, 2, -1), TAG, "tx reg");
    return i2c_master_receive(pt_i2c_dev, buf, len, -1);
}

static esp_err_t pt_touch_i2c_write(uint16_t reg, const void *buf, size_t len)
{
    uint8_t stackbuf[2 + 16];
    if (len <= sizeof(stackbuf) - 2)
    {
        stackbuf[0] = (uint8_t)(reg >> 8);
        stackbuf[1] = (uint8_t)reg;
        memcpy(&stackbuf[2], buf, len);
        return i2c_master_transmit(pt_i2c_dev, stackbuf, 2 + len, -1);
    }
    uint8_t *p = (uint8_t *)malloc(2 + len);
    if (!p)
        return ESP_ERR_NO_MEM;
    p[0] = (uint8_t)(reg >> 8);
    p[1] = (uint8_t)reg;
    memcpy(&p[2], buf, len);
    esp_err_t err = i2c_master_transmit(pt_i2c_dev, p, 2 + len, -1);
    free(p);
    return err;
}

/* --------- Public API --------- */
esp_err_t pt_touch_begin(void)
{
    // 1) Bus @ 100 kHz with internal pullups
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PT_GT911_I2C_SDA_GPIO,
        .scl_io_num = PT_GT911_I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &pt_i2c_bus), TAG, "new bus");

    // 2) Probe without reset
    uint8_t addr = 0;
    if (pt_touch_probe_addr(pt_i2c_bus, 0x14, 20))
    {
        addr = 0x14;
        ESP_LOGI(TAG, "ACK 0x14 (no reset)");
    }
    else if (pt_touch_probe_addr(pt_i2c_bus, 0x5D, 20))
    {
        addr = 0x5D;
        ESP_LOGI(TAG, "ACK 0x5D (no reset)");
    }
    else
    {
        // 3) Address-select reset: prefer 0x14
        pt_touch_addr_select(false);
        vTaskDelay(pdMS_TO_TICKS(80));
        if (pt_touch_probe_addr(pt_i2c_bus, 0x14, 50))
        {
            addr = 0x14;
            ESP_LOGI(TAG, "ACK 0x14 (reset)");
        }
        else
        {
            pt_touch_addr_select(true);
            vTaskDelay(pdMS_TO_TICKS(80));
            if (pt_touch_probe_addr(pt_i2c_bus, 0x5D, 50))
            {
                addr = 0x5D;
                ESP_LOGI(TAG, "ACK 0x5D (reset)");
            }
            else
                return ESP_ERR_NOT_FOUND;
        }
    }

    // 4) Add device @ 100 kHz initially
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(pt_i2c_bus, &dev_cfg, &pt_i2c_dev), TAG, "add dev");

    // 5) Prime read
    uint8_t st = 0;
    ESP_RETURN_ON_ERROR(pt_touch_i2c_read(PT_GT911_REG_STATUS, &st, 1), TAG, "prime STATUS");
    ESP_LOGI(TAG, "STATUS=0x%02X", st);
    ESP_LOGI(TAG, "PT_GT911 ready @ 0x%02X", addr);
    return ESP_OK;
}

bool pt_touch_i2c_ready(void)
{
    uint8_t st = 0;
    if (pt_i2c_dev && pt_touch_i2c_read(PT_GT911_REG_STATUS, &st, 1) == ESP_OK)
        return (st & 0x80) != 0;
    return false;
}

bool pt_touch_get_touch(pt_touch_event_t *ev)
{
    if (!ev)
        return false;
    ev->number = 0;

    uint8_t status = 0;
    if (pt_touch_i2c_read(PT_GT911_REG_STATUS, &status, 1) != ESP_OK)
        return false;
    if (!(status & 0x80))
        return false; // no new data

    uint8_t n = status & 0x0F;
    if (n == 0 || n > PT_GT911_MAX_POINTS)
    {
        uint8_t zero = 0;
        (void)pt_touch_i2c_write(PT_GT911_REG_STATUS, &zero, 1);
        return false;
    }

    uint8_t buf[PT_GT911_MAX_POINTS * 8];
    if (pt_touch_i2c_read(PT_GT911_REG_POINT1, buf, n * 8) != ESP_OK)
        return false;

    for (uint8_t i = 0; i < n; ++i)
    {
        const uint8_t *p = &buf[i * 8];
        ev->point[i].track_id = p[0];
        ev->point[i].x = ((uint16_t)p[2] << 8) | p[1];
        ev->point[i].y = ((uint16_t)p[4] << 8) | p[3];
        ev->point[i].size = ((uint16_t)p[6] << 8) | p[5];
        if (ev->point[i].x >= PT_GT911_MAX_X)
            ev->point[i].x = PT_GT911_MAX_X - 1;
        if (ev->point[i].y >= PT_GT911_MAX_X)
            ev->point[i].y = PT_GT911_MAX_Y - 1;
    }
    ev->number = n;

    uint8_t zero = 0;
    (void)pt_touch_i2c_write(PT_GT911_REG_STATUS, &zero, 1);
    return true;
}
