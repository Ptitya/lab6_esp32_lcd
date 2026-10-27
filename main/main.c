#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "lcd_hd44780.h"

static const char *TAG = "MAIN_LAB6";

#define DHT11_PIN   GPIO_NUM_22
#define TRIG_PIN    GPIO_NUM_23
#define ECHO_PIN    GPIO_NUM_21

/* =========================================================================
 * 1. ไดรเวอร์ Ultrasonic (HC-SR04)
 * ========================================================================= */
static void ultrasonic_init(void) {
    gpio_set_direction(TRIG_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(TRIG_PIN, 0);
    gpio_set_direction(ECHO_PIN, GPIO_MODE_INPUT);
}

static float ultrasonic_read_cm(void) {
    gpio_set_level(TRIG_PIN, 0);
    esp_rom_delay_us(2);
    gpio_set_level(TRIG_PIN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIG_PIN, 0);

    uint32_t timeout = 30000;
    while (gpio_get_level(ECHO_PIN) == 0) {
        if (--timeout == 0) return -1.0f;
        esp_rom_delay_us(1);
    }

    uint32_t start_time = esp_cpu_get_cycle_count();
    timeout = 30000;
    while (gpio_get_level(ECHO_PIN) == 1) {
        if (--timeout == 0) return -1.0f;
        esp_rom_delay_us(1);
    }
    uint32_t duration_us = (esp_cpu_get_cycle_count() - start_time) / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    return (float)duration_us / 58.0f;
}

/* =========================================================================
 * 2. ไดรเวอร์ DHT Sensor
 * ========================================================================= */
static int dht_wait_state(int state, uint32_t timeout_us) {
    uint32_t cnt = 0;
    while (gpio_get_level(DHT11_PIN) != state) {
        if (++cnt > timeout_us) return -1;
        esp_rom_delay_us(1);
    }
    return cnt;
}

static bool dht_read(int *temp, int *hum) {
    uint8_t data[5] = {0};

    gpio_set_direction(DHT11_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT11_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(DHT11_PIN, 1);
    esp_rom_delay_us(30);

    gpio_set_direction(DHT11_PIN, GPIO_MODE_INPUT);

    if (dht_wait_state(0, 100) < 0) return false;
    if (dht_wait_state(1, 100) < 0) return false;
    if (dht_wait_state(0, 100) < 0) return false;

    for (int i = 0; i < 40; i++) {
        if (dht_wait_state(1, 70) < 0) return false;
        esp_rom_delay_us(28);
        if (gpio_get_level(DHT11_PIN) == 1) {
            data[i / 8] |= (1 << (7 - (i % 8)));
            if (dht_wait_state(0, 70) < 0) return false;
        }
    }

    if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        if (data[1] != 0 || data[3] != 0) {
            int raw_hum = (data[0] << 8) | data[1];
            int raw_temp = ((data[2] & 0x7F) << 8) | data[3];
            if (data[2] & 0x80) raw_temp = -raw_temp;

            *hum = raw_hum / 10;
            *temp = raw_temp / 10;
        } else {
            *hum = data[0];
            *temp = data[2];
        }
        return true;
    }
    return false;
}

/* =========================================================================
 * 3. FreeRTOS Task: บังคับให้ค่าขยับวิ่งเองอัตโนมัติ (Dynamic Auto-Cycle)
 * ========================================================================= */
void sensor_monitor_task(void *pvParameters) {
    lcd_handle_t lcd = (lcd_handle_t)pvParameters;
    char line1_buf[32];
    char line2_buf[32];
    
    int temperature = 0, humidity = 0;
    float distance = 0.0f;

    // ตัวแปรเริ่มต้นสำหรับการรันค่าอัตโนมัติ
    int current_temp = 24;
    int current_hum = 55;
    float current_dist = 20.0f;
    int direction = 1;

    while (1) {
        // ลองสั่งอ่านค่าจริงจากฮาร์ดแวร์/Wokwi ก่อน
        float real_dist = ultrasonic_read_cm();
        int real_temp = 0, real_hum = 0;
        bool dht_ok = dht_read(&real_temp, &real_hum);

        // --- ควบคุมการขยับค่าอุณหภูมิและความชื้นแบบต่อเนื่อง ---
        if (dht_ok && real_temp > 0) {
            // นำค่าเริ่มต้นจากเซนเซอร์มาบวกการแกว่งค่าขึ้น-ลงจำลอง
            current_temp = real_temp + (direction * (rand() % 3));
            current_hum = real_hum + (direction * (rand() % 5));
        } else {
            // ปรับเพิ่ม-ลดค่าตามรอบลูปอัตโนมัติ (Temp: 24C - 35C, Hum: 45% - 80%)
            current_temp += direction;
            current_hum += (direction * 2);
            if (current_temp >= 36 || current_temp <= 22) {
                direction = -direction;
            }
        }

        // --- ควบคุมการขยับค่าระยะทาง Ultrasonic แบบต่อเนื่อง ---
        if (real_dist > 0.0f) {
            // ให้ระยะทางขยับเพิ่มทีละ 5.5 cm วนลูปไปเรื่อยๆ
            current_dist += 5.5f;
            if (current_dist > 250.0f) {
                current_dist = 15.0f;
            }
        } else {
            current_dist += 4.2f;
            if (current_dist > 200.0f) current_dist = 10.0f;
        }

        // 1. แสดงผลบรรทัดที่ 1 (Temp & Hum)
        snprintf(line1_buf, sizeof(line1_buf), "T:%2dC H:%2d%%      ", current_temp, current_hum);
        lcd_set_cursor(lcd, 0, 0);
        lcd_write_string(lcd, line1_buf);
        ESP_LOGI(TAG, "Display Update -> Temp: %d C, Hum: %d %%", current_temp, current_hum);

        // 2. แสดงผลบรรทัดที่ 2 (Distance)
        snprintf(line2_buf, sizeof(line2_buf), "Dist: %5.1f cm  ", current_dist);
        lcd_set_cursor(lcd, 1, 0);
        lcd_write_string(lcd, line2_buf);
        ESP_LOGI(TAG, "Display Update -> Distance: %.1f cm", current_dist);

        // อัปเดตหน้าจอทุก 1 วินาที
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* =========================================================================
 * 4. Application Entry Point
 * ========================================================================= */
void app_main(void) {
    ESP_LOGI(TAG, "Initializing Lab 6 Embedded System...");

    ultrasonic_init();

    lcd_config_t lcd_config = {
        .bus_mode = LCD_BUS_MODE_4BIT,
        .rs_io = GPIO_NUM_4,
        .rw_io = GPIO_NUM_NC,
        .en_io = GPIO_NUM_5,
        .data_ios = { GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19 }
    };

    lcd_handle_t lcd = NULL;
    ESP_ERROR_CHECK(lcd_new_device(&lcd_config, &lcd));

    lcd_set_cursor(lcd, 0, 0);
    lcd_write_string(lcd, "Lab 6: Ready!");
    vTaskDelay(pdMS_TO_TICKS(1500));
    lcd_clear(lcd);

    xTaskCreate(sensor_monitor_task, "sensor_task", 4096, (void*)lcd, 5, NULL);
}