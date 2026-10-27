#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "lcd_hd44780.h"

static const char *TAG = "lcd_hd44780";

struct lcd_hd44780_t {
    lcd_bus_mode_t bus_mode;
    gpio_num_t rs_io;
    gpio_num_t rw_io;
    gpio_num_t en_io;
    gpio_num_t data_ios[8];
    SemaphoreHandle_t lock;
};

static const uint8_t bar_patterns[5][8] = {
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10},
    {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18},
    {0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C},
    {0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E},
    {0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}
};

static inline void lcd_lock(lcd_handle_t lcd) {
    if (lcd->lock) {
        xSemaphoreTake(lcd->lock, portMAX_DELAY);
    }
}

static inline void lcd_unlock(lcd_handle_t lcd) {
    if (lcd->lock) {
        xSemaphoreGive(lcd->lock);
    }
}

static void lcd_toggle_enable(lcd_handle_t lcd) {
    gpio_set_level(lcd->en_io, 1);
    esp_rom_delay_us(1); // t_PWE >= 450 ns
    gpio_set_level(lcd->en_io, 0);
    esp_rom_delay_us(50);
}

static void lcd_write_bus_low_level(lcd_handle_t lcd, uint8_t value) {
    int num_pins = (lcd->bus_mode == LCD_BUS_MODE_8BIT) ? 8 : 4;
    for (int i = 0; i < num_pins; i++) {
        gpio_set_level(lcd->data_ios[i], (value >> i) & 0x01);
    }
    lcd_toggle_enable(lcd);
}

static void lcd_write_byte_unlocked(lcd_handle_t lcd, uint8_t is_data, uint8_t byte_val) {
    gpio_set_level(lcd->rs_io, is_data);
    if (lcd->rw_io != GPIO_NUM_NC) {
        gpio_set_level(lcd->rw_io, 0);
    }

    if (lcd->bus_mode == LCD_BUS_MODE_8BIT) {
        lcd_write_bus_low_level(lcd, byte_val);
    } else {
        lcd_write_bus_low_level(lcd, (byte_val >> 4) & 0x0F);
        lcd_write_bus_low_level(lcd, byte_val & 0x0F);
    }
}

esp_err_t lcd_new_device(const lcd_config_t *config, lcd_handle_t *ret_handle) {
    if (config == NULL || ret_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lcd_handle_t lcd = (lcd_handle_t)calloc(1, sizeof(struct lcd_hd44780_t));
    if (lcd == NULL) {
        return ESP_ERR_NO_MEM;
    }

    lcd->lock = xSemaphoreCreateMutex();
    if (lcd->lock == NULL) {
        free(lcd);
        return ESP_ERR_NO_MEM;
    }

    lcd->bus_mode = config->bus_mode;
    lcd->rs_io = config->rs_io;
    lcd->rw_io = config->rw_io;
    lcd->en_io = config->en_io;
    memcpy(lcd->data_ios, config->data_ios, sizeof(config->data_ios));

    uint64_t pin_mask = (1ULL << lcd->rs_io) | (1ULL << lcd->en_io);
    if (lcd->rw_io != GPIO_NUM_NC) {
        pin_mask |= (1ULL << lcd->rw_io);
    }

    int num_data_pins = (lcd->bus_mode == LCD_BUS_MODE_8BIT) ? 8 : 4;
    for (int i = 0; i < num_data_pins; i++) {
        pin_mask |= (1ULL << lcd->data_ios[i]);
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    if (lcd->rw_io != GPIO_NUM_NC) {
        gpio_set_level(lcd->rw_io, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    lcd_lock(lcd);
    // ลำดับขั้นตอน Software Reset ตาม Datasheet หน้า 45-46
    if (lcd->bus_mode == LCD_BUS_MODE_4BIT) {
        lcd_write_bus_low_level(lcd, 0x03);
        vTaskDelay(pdMS_TO_TICKS(5));
        lcd_write_bus_low_level(lcd, 0x03);
        esp_rom_delay_us(150);
        lcd_write_bus_low_level(lcd, 0x03);
        lcd_write_bus_low_level(lcd, 0x02); // ตั้งโหมด 4 บิต

        lcd_write_byte_unlocked(lcd, 0, 0x28); // 2 บรรทัด, ฟอนต์ 5x8
    } else {
        lcd_write_byte_unlocked(lcd, 0, 0x38);
        vTaskDelay(pdMS_TO_TICKS(5));
        lcd_write_byte_unlocked(lcd, 0, 0x38);
        esp_rom_delay_us(150);
        lcd_write_byte_unlocked(lcd, 0, 0x38);
        lcd_write_byte_unlocked(lcd, 0, 0x38);
    }

    lcd_write_byte_unlocked(lcd, 0, 0x0C); // เปิดการแสดงผล (Display On)
    lcd_write_byte_unlocked(lcd, 0, 0x01); // ล้างหน้าจอ (Clear Display)
    vTaskDelay(pdMS_TO_TICKS(2));
    lcd_unlock(lcd);

    *ret_handle = lcd;
    ESP_LOGI(TAG, "LCD (%s Mode) initialized successfully",
             lcd->bus_mode == LCD_BUS_MODE_8BIT ? "8-Bit" : "4-Bit");
    return ESP_OK;
}

esp_err_t lcd_del_device(lcd_handle_t handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;

    lcd_lock(handle);
    gpio_reset_pin(handle->rs_io);
    gpio_reset_pin(handle->en_io);
    if (handle->rw_io != GPIO_NUM_NC) gpio_reset_pin(handle->rw_io);

    int num_data_pins = (handle->bus_mode == LCD_BUS_MODE_8BIT) ? 8 : 4;
    for (int i = 0; i < num_data_pins; i++) {
        gpio_reset_pin(handle->data_ios[i]);
    }
    lcd_unlock(handle);

    if (handle->lock) {
        vSemaphoreDelete(handle->lock);
    }
    free(handle);
    return ESP_OK;
}

esp_err_t lcd_send_cmd(lcd_handle_t handle, uint8_t cmd) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    lcd_lock(handle);
    lcd_write_byte_unlocked(handle, 0, cmd);
    if (cmd == 0x01 || cmd == 0x02) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    lcd_unlock(handle);
    return ESP_OK;
}

esp_err_t lcd_send_data(lcd_handle_t handle, uint8_t data) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    lcd_lock(handle);
    lcd_write_byte_unlocked(handle, 1, data);
    lcd_unlock(handle);
    return ESP_OK;
}

esp_err_t lcd_set_cursor(lcd_handle_t handle, uint8_t line, uint8_t column) {
    if (handle == NULL || line > 1 || column > 15) return ESP_ERR_INVALID_ARG;
    uint8_t address = (line == 0) ? (0x00 + column) : (0x40 + column);
    return lcd_send_cmd(handle, 0x80 | address);
}

esp_err_t lcd_write_string(lcd_handle_t handle, const char *str) {
    if (handle == NULL || str == NULL) return ESP_ERR_INVALID_ARG;
    lcd_lock(handle);
    while (*str) {
        lcd_write_byte_unlocked(handle, 1, (uint8_t)(*str++));
    }
    lcd_unlock(handle);
    return ESP_OK;
}

esp_err_t lcd_clear(lcd_handle_t handle) {
    return lcd_send_cmd(handle, 0x01);
}

esp_err_t lcd_create_custom_char(lcd_handle_t handle, uint8_t location, const uint8_t charmap[8]) {
    if (handle == NULL || location > 7 || charmap == NULL) return ESP_ERR_INVALID_ARG;
    lcd_lock(handle);
    lcd_write_byte_unlocked(handle, 0, 0x40 | (location << 3));
    for (int i = 0; i < 8; i++) {
        lcd_write_byte_unlocked(handle, 1, charmap[i]);
    }
    lcd_write_byte_unlocked(handle, 0, 0x80);
    lcd_unlock(handle);
    return ESP_OK;
}

esp_err_t lcd_init_bargraph_glyphs(lcd_handle_t handle) {
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < 5; i++) {
        esp_err_t ret = lcd_create_custom_char(handle, i, bar_patterns[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_draw_bargraph(lcd_handle_t handle, uint8_t row, uint8_t start_col, uint8_t max_cols, uint8_t percentage) {
    if (handle == NULL || row > 1 || start_col >= 16) return ESP_ERR_INVALID_ARG;
    if (percentage > 100) percentage = 100;

    uint16_t total_pixels = max_cols * 5;
    uint16_t active_pixels = (percentage * total_pixels) / 100;

    uint8_t full_blocks = active_pixels / 5;
    uint8_t remainder = active_pixels % 5;

    lcd_lock(handle);
    uint8_t address = (row == 0) ? (0x00 + start_col) : (0x40 + start_col);
    lcd_write_byte_unlocked(handle, 0, 0x80 | address);

    for (uint8_t i = 0; i < max_cols; i++) {
        if (i < full_blocks) {
            lcd_write_byte_unlocked(handle, 1, 4);
        } else if (i == full_blocks && remainder > 0) {
            lcd_write_byte_unlocked(handle, 1, remainder - 1);
        } else {
            lcd_write_byte_unlocked(handle, 1, ' ');
        }
    }
    lcd_unlock(handle);
    return ESP_OK;
}