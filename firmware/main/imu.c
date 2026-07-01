// imu.c — QMI8658 wake-on-motion driver. Reuses the BSP I2C bus (shared with
// the touch controller / AXP2101). On motion the sensor's INT (GPIO17) fires,
// a task calls idle_notify_activity() to wake the screen. See the design spec.
//
// Register values marked "tune on-device" are the best-known QMI8658 WoM recipe;
// confirm them against the QMI8658 datasheet during bring-up (the WHO_AM_I probe
// below de-risks the I2C address first).
#include "imu.h"
#include "idle.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "imu";

// --- QMI8658 I2C + registers -------------------------------------------------
#define QMI8658_I2C_ADDR   0x6B   // SA0=1 on this board; try 0x6A if WHO_AM_I fails
#define QMI8658_WHO_AM_I   0x00   // expect 0x05
#define QMI8658_WHOAMI_VAL 0x05
#define QMI8658_CTRL1      0x02   // serial IF / addr auto-increment
#define QMI8658_CTRL2      0x03   // accel: ODR + full-scale
#define QMI8658_CTRL7      0x08   // sensor enable (aEN = bit0)
#define QMI8658_CTRL8      0x09   // motion engine / INT selection
#define QMI8658_CTRL9      0x0A   // host command register
#define QMI8658_CAL1_L     0x0B   // command arg: WoM threshold (mg)
#define QMI8658_CAL1_H     0x0C   // command arg: INT map / blanking
#define QMI8658_STATUSINT  0x2D   // bit0 = ctrl9 command done

// CTRL9 host commands
#define QMI8658_CMD_WOM    0x08   // CTRL_CMD_WRITE_WOM_SETTING
#define QMI8658_CMD_ACK    0x00   // CTRL_CMD_ACK

// Config values (tune on-device against the datasheet)
#define QMI8658_CTRL2_ACC  0x03   // low ODR, moderate full-scale for WoM
#define QMI8658_WOM_THRESH 0x20   // motion threshold in mg (~32 mg); raise to desensitize
#define QMI8658_WOM_INTCFG 0x00   // INT map / blanking

#define IMU_INT_GPIO       GPIO_NUM_17

static i2c_master_dev_handle_t s_dev;
static SemaphoreHandle_t       s_motion_sem;

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

// ISR: just release the semaphore; all I2C / logic runs in the task.
static void IRAM_ATTR imu_isr(void *arg)
{
    (void)arg;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_motion_sem, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void motion_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_motion_sem, portMAX_DELAY) == pdTRUE) {
            idle_notify_activity();
        }
    }
}

void imu_start(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = QMI8658_I2C_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "add device failed; motion wake disabled");
        return;
    }

    uint8_t who = 0;
    if (rd(QMI8658_WHO_AM_I, &who) != ESP_OK || who != QMI8658_WHOAMI_VAL) {
        ESP_LOGW(TAG, "QMI8658 not found (WHO_AM_I=0x%02x); motion wake disabled", who);
        return;
    }
    ESP_LOGI(TAG, "QMI8658 detected (WHO_AM_I=0x%02x)", who);

    // Configure accel + wake-on-motion via the CTRL9 command interface.
    wr(QMI8658_CTRL1, 0x60);              // serial IF: addr auto-increment, little-endian
    wr(QMI8658_CTRL7, 0x00);              // disable sensors while configuring
    wr(QMI8658_CTRL2, QMI8658_CTRL2_ACC);// accel ODR + full-scale
    wr(QMI8658_CAL1_L, QMI8658_WOM_THRESH);
    wr(QMI8658_CAL1_H, QMI8658_WOM_INTCFG);
    wr(QMI8658_CTRL9, QMI8658_CMD_WOM);  // apply WoM setting

    // Wait for the command-done handshake, then ack.
    for (int i = 0; i < 10; i++) {
        uint8_t st = 0;
        if (rd(QMI8658_STATUSINT, &st) == ESP_OK && (st & 0x01)) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    wr(QMI8658_CTRL9, QMI8658_CMD_ACK);
    wr(QMI8658_CTRL7, 0x01);              // enable accel (low-power WoM mode)

    // Motion signal path: ISR -> semaphore -> task -> idle_notify_activity().
    s_motion_sem = xSemaphoreCreateBinary();
    if (!s_motion_sem) { ESP_LOGE(TAG, "sem alloc failed"); return; }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << IMU_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,   // flip to POSEDGE if INT never fires
    };
    gpio_config(&io);
    gpio_install_isr_service(0);          // harmless (INVALID_STATE) if already installed
    gpio_isr_handler_add(IMU_INT_GPIO, imu_isr, NULL);

    if (xTaskCreate(motion_task, "imu", 2560, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create motion task");
        return;
    }
    ESP_LOGI(TAG, "QMI8658 wake-on-motion armed (thr=0x%02x)", QMI8658_WOM_THRESH);
}
