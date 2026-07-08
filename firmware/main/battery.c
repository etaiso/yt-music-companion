// battery.c — AXP2101 fuel-gauge reader. Reuses the BSP I2C bus (shared with the
// touch controller). Polls every 10 s and caches a lock-protected snapshot.
#include "battery.h"
#include "axp2101_decode.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define POLL_MS 10000

static battery_status_t s_status;                 // last reading
static portMUX_TYPE     s_mux = portMUX_INITIALIZER_UNLOCKED;
static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static void poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t s1 = 0, s2 = 0, soc = 0;
        esp_err_t e1 = rd(AXP2101_REG_STATUS1, &s1);
        esp_err_t e2 = rd(AXP2101_REG_STATUS2, &s2);
        esp_err_t e3 = rd(AXP2101_REG_BAT_PERCENT, &soc);

        if (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK) {
            battery_status_t b;
            axp2101_decode(s1, s2, soc, &b);
            portENTER_CRITICAL(&s_mux);
            s_status = b;
            portEXIT_CRITICAL(&s_mux);
        } else {
            ESP_LOGW(TAG, "AXP2101 read failed (%d/%d/%d)", e1, e2, e3);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void battery_start(void)
{
    // Reuse the BSP-initialized I2C master bus (shared with the touch controller).
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &cfg, &s_dev));

    // Enable the fuel gauge (reg 0x18 bit 3) so reg 0xA4 reports SOC.
    uint8_t cfgreg = 0;
    if (rd(AXP2101_REG_GAUGE_CFG, &cfgreg) == ESP_OK) {
        uint8_t buf[2] = { AXP2101_REG_GAUGE_CFG, (uint8_t)(cfgreg | (1u << 3)) };
        if (i2c_master_transmit(s_dev, buf, 2, 100) != ESP_OK) {
            ESP_LOGW(TAG, "AXP2101 fuel-gauge enable failed");
        }
    }

    if (xTaskCreate(poll_task, "battery", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create battery poll task");
        return;
    }
    ESP_LOGI(TAG, "AXP2101 battery poll started (%d ms)", POLL_MS);
}

void battery_get(battery_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_status;
    portEXIT_CRITICAL(&s_mux);
}

bool battery_power_off(void)
{
    uint8_t cfg = 0;
    if (rd(AXP2101_REG_COMMON_CFG, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 power-off: read COMMON_CFG failed");
        return false;
    }
    uint8_t buf[2] = { AXP2101_REG_COMMON_CFG, (uint8_t)(cfg | 0x01u) };
    if (i2c_master_transmit(s_dev, buf, 2, 100) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 power-off: write failed");
        return false;               // stay on; caller retries next tick
    }
    ESP_LOGI(TAG, "AXP2101 soft power-off issued");
    return true;
}
