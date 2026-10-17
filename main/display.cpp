// SSD1309 OLED backend. The M5Stick target carries an ST7789 instead;
// its backend lives in display_st7789.c.

#include "sdkconfig.h"

#if !CONFIG_NUCULA_BOARD_M5STICK

#include "display.h"

#include <cstring>
#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"

#define TAG "display"

#define SSD1309_ADDR  BOARD_OLED_ADDR
#define SSD1309_CMD   0x00   // control byte: following bytes are commands
#define SSD1309_DATA  0x40   // control byte: following bytes are GDDRAM data

#define PIN_RST  BOARD_OLED_RST_PIN

static i2c_master_dev_handle_t s_dev = nullptr;

// Page-format framebuffer: 8 pages × 128 columns = 1024 bytes.
// Each byte holds 8 vertical pixels; bit 0 = topmost in the page.
static uint8_t s_fb[LCD_H / 8 * LCD_W];

// Transmit buffer: one control byte + framebuffer (static to avoid stack use)
static uint8_t s_tx[1 + sizeof(s_fb)];


// -------------------------------------------------------------------------
// SSD1309 I2C helpers
// -------------------------------------------------------------------------

static void ssd1309_send_cmds(const uint8_t *cmds, size_t len)
{
    // Prepend command control byte
    uint8_t buf[32];
    buf[0] = SSD1309_CMD;
    size_t chunk;
    while (len > 0) {
        chunk = (len > sizeof(buf) - 1) ? sizeof(buf) - 1 : len;
        memcpy(buf + 1, cmds, chunk);
        i2c_master_transmit(s_dev, buf, chunk + 1, 100);
        cmds += chunk;
        len  -= chunk;
    }
}

// -------------------------------------------------------------------------
// Init
// -------------------------------------------------------------------------

void display_init(i2c_master_bus_handle_t bus)
{
#if CONFIG_NUCULA_BOARD_NUCULA_V2
    // Headless prototype bring-up: hold OLED boost off and reset asserted.
    gpio_set_level(BOARD_OLED_POWER_PIN, 0);
    gpio_set_direction(BOARD_OLED_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RST, 1);
    gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
    ESP_LOGW(TAG, "display disconnected: OLED power off");
    return;
#endif

    // Legacy display initialization below. Before enabling the v2 panel,
    // implement its inverted reset and switched boost sequencing.
    if (!bus) {
        ESP_LOGW(TAG, "no I2C bus, display disabled");
        return;
    }

    // Optional hardware reset
    if ((int)PIN_RST >= 0) {
        gpio_reset_pin(PIN_RST);
        gpio_set_direction(PIN_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(PIN_RST, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Probe first: on a board without the display every later transmit
    // would just NACK-spam the shared bus.
    if (i2c_master_probe(bus, SSD1309_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "no display at 0x%02X, display disabled", SSD1309_ADDR);
        return;
    }

    // Add SSD1309 as device on the shared I2C bus
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = SSD1309_ADDR;
    dev_cfg.scl_speed_hz    = 100000;

    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1309 add failed: %s", esp_err_to_name(err));
        s_dev = nullptr;
        return;
    }

    // SSD1309 initialisation (compatible with SSD1306 modules too)
    static const uint8_t init[] = {
        0xAE,              // display off
        0xD5, 0x80,        // clock divide ratio
        0xA8, 0x3F,        // multiplex 64
        0xD3, 0x00,        // display offset 0
        0x40,              // start line 0
        0x8D, 0x14,        // charge pump enable
        0x20, 0x00,        // horizontal addressing mode
        0xA1,              // segment remap (mirror X)
        0xC8,              // COM scan reversed (mirror Y)
        0xDA, 0x12,        // COM pins: alternative, no LR remap
        0x81, 0xCF,        // contrast
        0xD9, 0xF1,        // pre-charge period
        0xDB, 0x40,        // VCOMH deselect level
        0xA4,              // output follows RAM
        0xA6,              // normal display (not inverted)
        0xAF,              // display on
    };
    ssd1309_send_cmds(init, sizeof(init));

    memset(s_fb, 0, sizeof(s_fb));
    display_update();

    ESP_LOGI(TAG, "SSD1309 initialized (%dx%d I2C 0x%02X)", LCD_W, LCD_H, SSD1309_ADDR);
}

// -------------------------------------------------------------------------
// Framebuffer → display transfer
// -------------------------------------------------------------------------

void display_update(void)
{
    if (!s_dev) return;

    // Set draw window to full screen
    static const uint8_t window[] = {
        0x21, 0x00, 0x7F,   // column range 0–127
        0x22, 0x00, 0x07,   // page range 0–7
    };
    ssd1309_send_cmds(window, sizeof(window));

    // Push framebuffer
    s_tx[0] = SSD1309_DATA;
    memcpy(s_tx + 1, s_fb, sizeof(s_fb));
    i2c_master_transmit(s_dev, s_tx, sizeof(s_tx), 200);
}

void display_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

// -------------------------------------------------------------------------
// Drawing primitives
// -------------------------------------------------------------------------

void display_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    if (on)
        s_fb[(y >> 3) * LCD_W + x] |=  (1 << (y & 7));
    else
        s_fb[(y >> 3) * LCD_W + x] &= ~(1 << (y & 7));
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    int x2 = x + w, y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > LCD_W) x2 = LCD_W;
    if (y2 > LCD_H) y2 = LCD_H;
    if (x2 <= x || y2 <= y) return;

    bool on = (color != 0);
    for (int row = y; row < y2; row++) {
        uint8_t mask = 1 << (row & 7);
        int page_off = (row >> 3) * LCD_W;
        if (on) {
            for (int col = x; col < x2; col++)
                s_fb[page_off + col] |= mask;
        } else {
            uint8_t nmask = ~mask;
            for (int col = x; col < x2; col++)
                s_fb[page_off + col] &= nmask;
        }
    }
}

void display_hline(int x, int y, int w)
{
    display_fill_rect(x, y, w, 1, COLOR_WHITE);
}


#endif // !CONFIG_NUCULA_BOARD_M5STICK
