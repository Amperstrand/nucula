// ST7789 display for the M5StickC Plus via the IDF esp_lcd panel stack
// (panel IO + built-in ST7789 driver).
//
// Power sequencing: on the Plus nothing runs until the AXP192 PMU
// (I2C1, 0x34) turns its rails on — display_st7789_init calls
// axp192_init() and treats a probe failure as the Plus2 variant, whose
// rails are GPIO-driven: GPIO4 holds the regulator on and GPIO27 gates
// the backlight. The two variants also wire the panel differently
// (DC/RST move to G14/G12 on the Plus2), resolved at init time.

#include "display_st7789.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "board.h"
#include "axp192.h"
#include "display.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
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

// Plus2 wiring and regulator hold, per the M5Stack Plus2 schematic.
#define PLUS2_LCD_DC_PIN   GPIO_NUM_14
#define PLUS2_LCD_RST_PIN  GPIO_NUM_12
#define PLUS2_POWER_HOLD   GPIO_NUM_4

static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_draw_done;
static st7789_variant_t s_variant = ST7789_VARIANT_NONE;

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

static void backlight_gpio(bool on)
{
    gpio_set_level(BOARD_LCD_BL_PIN, on ? 1 : 0);
}

esp_err_t display_st7789_init(void)
{
    if (s_panel)
        return ESP_OK;

    if (axp192_init() == ESP_OK) {
        s_variant = ST7789_VARIANT_PLUS;
    } else {
        s_variant = ST7789_VARIANT_PLUS2;
        // Regulator hold: without GPIO4 high the Plus2 powers itself
        // down after a power-button wake.
        gpio_config_t hold = {
            .pin_bit_mask = 1ULL << PLUS2_POWER_HOLD,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&hold);
        gpio_set_level(PLUS2_POWER_HOLD, 1);
        gpio_config_t bl = {
            .pin_bit_mask = 1ULL << BOARD_LCD_BL_PIN,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&bl);
        backlight_gpio(false);
    }

    // Backlight stays dark until the panel is configured.
    display_st7789_backlight(false);

    vTaskDelay(pdMS_TO_TICKS(200)); // let the rails settle

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = BOARD_LCD_MOSI_PIN,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_LCD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_MAX_XFER,
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
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
        .dc_gpio_num = s_variant == ST7789_VARIANT_PLUS2 ? PLUS2_LCD_DC_PIN
                                                          : BOARD_LCD_DC_PIN,
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
        .reset_gpio_num = s_variant == ST7789_VARIANT_PLUS2 ? PLUS2_LCD_RST_PIN
                                                            : BOARD_LCD_RST_PIN,
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
    // Quirks of this glass: colors are inverted, and the 135x240 window
    // sits at (52, 40) in the controller's memory.
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y);
    esp_lcd_panel_disp_on_off(s_panel, true);

    ESP_LOGI(TAG, "ST7789 %dx%d ready (%s variant, offset %d,%d, inverted)",
             BOARD_LCD_WIDTH, BOARD_LCD_HEIGHT,
             s_variant == ST7789_VARIANT_PLUS ? "Plus" : "Plus2",
             BOARD_LCD_OFFSET_X, BOARD_LCD_OFFSET_Y);
    return ESP_OK;
}

st7789_variant_t display_st7789_variant(void)
{
    return s_variant;
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
    if (s_variant == ST7789_VARIANT_PLUS2)
        backlight_gpio(on);
    else
        axp192_backlight(on);
}

// -------------------------------------------------------------------------
// Monochrome display.h backend: a 1bpp linear canvas that blits to the
// panel as RGB565 scanlines on display_update(). White = lit pixel.
// -------------------------------------------------------------------------

#define MONO_BYTES (((LCD_H * LCD_W) + 7) / 8)

static uint8_t s_mono[MONO_BYTES];
static uint8_t s_line[LCD_MAX_XFER];

static inline bool mono_get(int x, int y)
{
    size_t bit = (size_t)y * LCD_W + x;
    return (s_mono[bit / 8] >> (bit % 8)) & 1;
}

static inline void mono_set(int x, int y, bool on)
{
    size_t bit = (size_t)y * LCD_W + x;
    if (on)
        s_mono[bit / 8] |= (uint8_t)(1 << (bit % 8));
    else
        s_mono[bit / 8] &= (uint8_t)~(1 << (bit % 8));
}

void display_clear(void)
{
    memset(s_mono, 0, sizeof(s_mono));
}

void display_pixel(int x, int y, bool on)
{
    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H)
        return;
    mono_set(x, y, on);
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    bool on = (color != COLOR_BLACK);
    for (int r = y; r < y + h; r++)
        for (int c = x; c < x + w; c++)
            display_pixel(c, r, on);
}

void display_hline(int x, int y, int w)
{
    display_fill_rect(x, y, w, 1, COLOR_WHITE);
}

void display_update(void)
{
    if (!s_panel)
        return;

    for (int y = 0; y < LCD_H; y++) {
        for (int x = 0; x < LCD_W; x++) {
            s_line[2 * x]     = mono_get(x, y) ? 0xFF : 0x00;
            s_line[2 * x + 1] = mono_get(x, y) ? 0xFF : 0x00;
        }
        if (!draw_sync(0, y, LCD_W, y + 1, s_line))
            return;
    }
}

#endif // CONFIG_NUCULA_BOARD_M5STICK
