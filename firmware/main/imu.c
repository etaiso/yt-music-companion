// imu.c — QMI8658 wake-on-motion driver. Reuses the BSP I2C bus (shared with
// the touch controller / AXP2101). On motion the sensor's INT fires, a task
// calls idle_notify_activity() to wake the screen. See the design spec.
//
// WoM register recipe follows the QMI8658C datasheet + the SensorLib reference
// driver. The board wires BOTH QMI_INT1 (GPIO17) and QMI_INT2 (GPIO21) to the
// IMU; we route WoM to INT1 but watch BOTH pins so the exact schematic mapping
// doesn't matter — whichever line the sensor drives, we catch it.
#include "imu.h"
#include "idle.h"

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "imu";

// --- QMI8658 I2C + registers -------------------------------------------------
#define QMI8658_I2C_ADDR   0x6B   // SA0=1 on this board (WHO_AM_I confirmed)
#define QMI8658_WHO_AM_I   0x00   // expect 0x05
#define QMI8658_WHOAMI_VAL 0x05
#define QMI8658_CTRL1      0x02   // serial IF / INT pin enables / addr auto-inc
#define QMI8658_CTRL2      0x03   // accel: full-scale + ODR
#define QMI8658_CTRL7      0x08   // sensor enable (aEN = bit0)
#define QMI8658_CTRL9      0x0A   // host command register
#define QMI8658_CAL1_L     0x0B   // command arg: WoM threshold (mg)
#define QMI8658_CAL1_H     0x0C   // command arg: INT select/polarity + blanking
#define QMI8658_STATUSINT  0x2D   // bit7 = CTRL9 command done

// CTRL9 host commands
#define QMI8658_CMD_WOM    0x08   // CTRL_CMD_WRITE_WOM_SETTING
#define QMI8658_CMD_ACK    0x00   // CTRL_CMD_ACK

// Config values (tune on-device)
//  CTRL1: bit6 ADDR_AI (auto-increment) + bit3 INT1_EN (drive the INT1 pin).
//  CTRL2: bits[6:4]=010 (+/-8g) | bits[3:0]=1100 (low-power 128Hz ODR).
//  CAL1_H: bits[7:6]=10 -> WoM on INT1, initial level HIGH (so motion pulls it
//          LOW -> falling edge); bits[5:0]=0x20 blanking samples.
//  WOM_THRESH: motion threshold in mg (1 LSB = 1mg, 8-bit). Higher = less
//  sensitive. 0x80 (~128mg) is a middle ground: max (0xFF/255mg) was too hard
//  to wake, 0x20 (32mg) too twitchy. WoM triggers on a single sample over
//  threshold, so it can't fully reject sharp impulses (desk knocks) by
//  threshold alone — a software sustained-motion filter would be needed for that.
#define QMI8658_CTRL1_CFG   0x48
#define QMI8658_CTRL2_ACC   0x2C
#define QMI8658_WOM_INTCFG  0xA0
#define QMI8658_WOM_THRESH  0x80

#define IMU_INT1_GPIO      GPIO_NUM_17
#define IMU_INT2_GPIO      GPIO_NUM_21

static i2c_master_dev_handle_t s_dev;
static QueueHandle_t           s_motion_q;   // carries the GPIO num that fired

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

// Poll STATUSINT bit7 for the CTRL9 command handshake (set=done, or clear).
static void wait_cmd_done(bool want_set)
{
    for (int i = 0; i < 50; i++) {
        uint8_t st = 0;
        if (rd(QMI8658_STATUSINT, &st) == ESP_OK && (bool)(st & 0x80) == want_set) return;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ISR: just queue which pin fired; all I2C / logging runs in the task.
static void IRAM_ATTR imu_isr(void *arg)
{
    uint32_t gpio = (uint32_t)(uintptr_t)arg;
    BaseType_t hp = pdFALSE;
    xQueueSendFromISR(s_motion_q, &gpio, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void motion_task(void *arg)
{
    (void)arg;
    uint32_t gpio;
    for (;;) {
        if (xQueueReceive(s_motion_q, &gpio, portMAX_DELAY) == pdTRUE) {
            (void)gpio;                 // which pin fired doesn't matter — any INT = activity
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
        i2c_master_bus_rm_device(s_dev);
        return;
    }
    ESP_LOGI(TAG, "QMI8658 detected (WHO_AM_I=0x%02x)", who);

    // --- Configure wake-on-motion via the CTRL9 command interface ------------
    wr(QMI8658_CTRL7, 0x00);                  // disable sensors while configuring
    wr(QMI8658_CTRL2, QMI8658_CTRL2_ACC);     // accel full-scale + low-power ODR
    wr(QMI8658_CAL1_L, QMI8658_WOM_THRESH);   // WoM threshold (mg)
    wr(QMI8658_CAL1_H, QMI8658_WOM_INTCFG);   // WoM -> INT1, idle-high, blanking
    wr(QMI8658_CTRL9, QMI8658_CMD_WOM);       // apply WoM setting
    wait_cmd_done(true);                      // command accepted
    wr(QMI8658_CTRL9, QMI8658_CMD_ACK);       // acknowledge
    wait_cmd_done(false);                     // handshake cleared
    wr(QMI8658_CTRL7, 0x01);                  // enable accel (low-power WoM mode)
    wr(QMI8658_CTRL1, QMI8658_CTRL1_CFG);     // enable the INT1 output pin

    // --- Motion signal path: ISR -> queue -> task -> idle_notify_activity() --
    s_motion_q = xQueueCreate(4, sizeof(uint32_t));
    if (!s_motion_q) {
        ESP_LOGE(TAG, "queue alloc failed");
        i2c_master_bus_rm_device(s_dev);
        return;
    }

    // Watch BOTH IMU INT lines with ANYEDGE. On this board the sensor drives
    // INT1 (GPIO17, confirmed on-device), but watching both pins on any edge
    // keeps wake robust to the INT routing/polarity — each edge just flags
    // activity, so the extra interrupts are effectively free.
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << IMU_INT1_GPIO) | (1ULL << IMU_INT2_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io);
    // Don't call gpio_install_isr_service() — the BSP already installs it (touch
    // runs in IRQ mode), and calling it again logs a benign but alarming red
    // error at boot. Just attach our handlers to the existing service.
    gpio_isr_handler_add(IMU_INT1_GPIO, imu_isr, (void *)(uintptr_t)IMU_INT1_GPIO);
    gpio_isr_handler_add(IMU_INT2_GPIO, imu_isr, (void *)(uintptr_t)IMU_INT2_GPIO);

    if (xTaskCreate(motion_task, "imu", 2560, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create motion task");
        i2c_master_bus_rm_device(s_dev);
        return;
    }
    ESP_LOGI(TAG, "QMI8658 wake-on-motion armed (thr=0x%02x)", QMI8658_WOM_THRESH);
}
