// ST7789 display for the M5StickC Plus via the IDF esp_lcd panel
// stack (panel IO + built-in ST7789 driver) — replaces the earlier
// raw-SPI command driver.
//
// Power sequencing note: nothing on this board runs until the AXP192 PMU
// (I2C1, 0x34) turns its rails on. Register values are cross-checked
// against the official M5Stack M5StickC-Plus AXP192 driver and the
// bolty-rs board-m5stick init, both hardware-verified on this panel:
//   reg 0x28 = 0xCC  LDO2 (backlight) + LDO3 (LCD logic) at 3.0V
//   reg 0x12 |= 0x4D DC-DC1 + LDO2 + LDO3 + EXTEN on
//   reg 0x10 |= 0x04 EXTEN boost — feeds the Grove port the MFRC522
//                    draws its power from; clearing it kills the reader

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "display_st7789.h"
#include "board.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TAG "display"

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SPI_HZ     (10 * 1000 * 1000) // panel does 20MHz; 10MHz for reliability
#define LCD_MAX_XFER   (BOARD_LCD_WIDTH * 2) // one 16bpp scanline

static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_draw_done;

// draw_bitmap queues its color transfer asynchronously; the panel-IO
// callback signals completion, which is what keeps the caller's line
// buffer valid until the SPI engine has consumed it.
static bool IRAM_ATTR on_draw_done(esp_lcd_panel_io_handle_t io,
                                   esp_lcd_panel_io_event_data_t *edata,
                                   void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_draw_done, &woken);
    return woken == pdTRUE;
}

// One buffer-owning draw: drain any stale completion token, queue the
// transfer, then wait for its callback. Returns false when the wait
// times out (queue stall) — callers stop drawing.
static bool draw_sync(int x0, int y0, int x1, int y1, const void *color)
{
    xSemaphoreTake(s_draw_done, 0);
    if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, color) != ESP_OK)
        return false;
    return xSemaphoreTake(s_draw_done, pdMS_TO_TICKS(100)) == pdTRUE;
}

// One-shot I2C1 session with the AXP192; the PMU keeps its register
// state, so the bus is torn down again afterwards.
static esp_err_t axp_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), 100);
}

static esp_err_t axp_rmw(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask)
{
    uint8_t cur;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &cur, 1, 100);
    if (err != ESP_OK)
        return err;
    if ((cur & mask) == mask)
        return ESP_OK;
    return axp_write(dev, reg, cur | mask);
}

static esp_err_t axp192_power_enable(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_1,
        .sda_io_num        = BOARD_PMU_SDA_PIN,
        .scl_io_num        = BOARD_PMU_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK)
        return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_PMU_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err == ESP_OK)
        err = axp_write(dev, 0x28, 0xCC);        // LDO2/LDO3 = 3.0V
    if (err == ESP_OK)
        err = axp_rmw(dev, 0x12, 0x4D);          // DC-DC1 + LDO2 + LDO3 + EXTEN
    if (err == ESP_OK)
        err = axp_rmw(dev, 0x10, 0x04);          // EXTEN: Grove 5V boost
    if (dev)
        i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    return err;
}

// EXTEN (reg 0x10 bit 2) gates the Grove rail that powers the MFRC522.
// The PMU holds this rail across ESP32 resets, so a latched RC522
// (SDA stuck LOW, immune to SCL clocking) only clears when the rail
// actually drops — hence this power-cycle helper.
esp_err_t axp192_grove_power(bool on)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = I2C_NUM_1,
        .sda_io_num        = BOARD_PMU_SDA_PIN,
        .scl_io_num        = BOARD_PMU_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK)
        return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_PMU_ADDR,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err == ESP_OK) {
        uint8_t reg = 0x10, cur = 0;
        err = i2c_master_transmit_receive(dev, &reg, 1, &cur, 1, 100);
        if (err == ESP_OK) {
            uint8_t want = on ? (uint8_t)(cur | 0x04)
                              : (uint8_t)(cur & ~0x04);
            if (want != cur)
                err = axp_write(dev, 0x10, want);
        }
    }
    if (dev)
        i2c_master_bus_rm_device(dev);
    i2c_del_master_bus(bus);
    return err;
}

esp_err_t display_st7789_init(void)
{
    if (s_panel)
        return ESP_OK;

    // Keep the backlight dark until the panel is configured. DC and RST
    // belong to the esp_lcd panel IO and driver now; only the backlight
    // stays manual.
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << BOARD_LCD_BL_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(BOARD_LCD_BL_PIN, 0);

    esp_err_t err = axp192_power_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP192 power sequencing failed: %s — display disabled",
                 esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // let the rails settle

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_LCD_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_MAX_XFER,
    };
    err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_draw_done = xSemaphoreCreateBinary();
    if (!s_draw_done) {
        spi_bus_free(LCD_SPI_HOST);
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC_PIN,
        .cs_gpio_num = BOARD_LCD_CS_PIN,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 3,
        .on_color_trans_done = on_draw_done,
    };
    esp_lcd_panel_io_handle_t io_handle = NULL;
    err = esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_cfg, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel IO init failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_draw_done);
        s_draw_done = NULL;
        spi_bus_free(LCD_SPI_HOST);
        return err;
    }

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_PIN,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &dev_cfg, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel create failed: %s", esp_err_to_name(err));
        vSemaphoreDelete(s_draw_done);
        s_draw_done = NULL;
        spi_bus_free(LCD_SPI_HOST);
        return err;
    }

    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    // Quirks verified on this glass: colors are inverted, and the
    // 135x240 window sits at (52, 40) in the controller's memory.
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "ST7789 %dx%d ready via esp_lcd (offset %d,%d, inverted)",
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
             BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y);
    return ESP_OK;
}

void display_st7789_fill(uint16_t color)
{
    display_st7789_fill_rect(0, 0, BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT, color);
}

void display_st7789_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > BOARD_LCD_WIDTH || y + h > BOARD_LCD_HEIGHT)
        return;

    // Colors go out MSB first (RGB565 big-endian on the wire); one
    // scanline pattern is streamed row by row, each transfer owned
    // until its completion callback fires.
    static uint8_t line[LCD_MAX_XFER];
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    for (int i = 0; i < w; i++) {
        line[2 * i] = hi;
        line[2 * i + 1] = lo;
    }
    for (int r = 0; r < h; r++) {
        if (!draw_sync(x, y + r, x + w, y + r + 1, line))
            return;
    }
}

void display_st7789_set_pixel(int x, int y, uint16_t color)
{
    if (!s_panel || x < 0 || y < 0 ||
        x >= BOARD_LCD_WIDTH || y >= BOARD_LCD_HEIGHT)
        return;

    uint8_t px[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    draw_sync(x, y, x + 1, y + 1, px);
}

void display_st7789_backlight(bool on)
{
    gpio_set_level(BOARD_LCD_BL_PIN, on ? 1 : 0);
}

#endif // CONFIG_NUCULA_BOARD_M5STICK
