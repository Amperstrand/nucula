// Minimal ST7789 driver for the M5StickC Plus panel — raw SPI commands,
// no external display library.
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "display"

#define ST_CMD_SWRESET 0x01
#define ST_CMD_SLPOUT   0x11
#define ST_CMD_NORON    0x13
#define ST_CMD_INVON    0x21
#define ST_CMD_DISPON   0x29
#define ST_CMD_CASET    0x2A
#define ST_CMD_RASET    0x2B
#define ST_CMD_RAMWR    0x2C
#define ST_CMD_MADCTL   0x36
#define ST_CMD_COLMOD   0x3A

#define LCD_SPI_HOST   SPI2_HOST
#define LCD_SPI_HZ     (10 * 1000 * 1000) // panel does 20MHz; 10MHz for reliability
#define LCD_MAX_XFER   (BOARD_LCD_WIDTH * 2) // one 16bpp scanline

static spi_device_handle_t s_spi = NULL;

// Command phase (DC low) then optional data phase (DC high).
static void lcd_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(BOARD_LCD_DC_PIN, 0);
    spi_device_polling_transmit(s_spi, &t);
    if (len == 0)
        return;
    spi_transaction_t d = {
        .length = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(BOARD_LCD_DC_PIN, 1);
    spi_device_polling_transmit(s_spi, &d);
}

// Address window in panel coordinates — the (52, 40) offset maps the
// 135x240 window onto this specific glass.
static void lcd_set_window(int x, int y, int w, int h)
{
    int x0 = x + BOARD_LCD_OFFSET_X;
    int x1 = x0 + w - 1;
    int y0 = y + BOARD_LCD_OFFSET_Y;
    int y1 = y0 + h - 1;
    uint8_t ca[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF),
    };
    uint8_t ra[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF),
    };
    lcd_cmd(ST_CMD_CASET, ca, 4);
    lcd_cmd(ST_CMD_RASET, ra, 4);
}

// Streams `lines` copies of a prepared scanline after RAMWR. Colors go
// out MSB first (RGB565 big-endian on the wire).
static void lcd_stream(const uint8_t *line, int len, int lines)
{
    lcd_cmd(ST_CMD_RAMWR, NULL, 0);
    gpio_set_level(BOARD_LCD_DC_PIN, 1);
    for (int i = 0; i < lines; i++) {
        spi_transaction_t t = {
            .length = len * 8,
            .tx_buffer = line,
        };
        spi_device_polling_transmit(s_spi, &t);
    }
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
    if (s_spi)
        return ESP_OK;

    // Keep the backlight dark until the panel is configured.
    gpio_num_t outs[] = { BOARD_LCD_DC_PIN, BOARD_LCD_RST_PIN, BOARD_LCD_BL_PIN };
    for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << outs[i],
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(outs[i], 0);
    }

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
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = BOARD_LCD_CS_PIN,
        .queue_size = 1,
    };
    err = spi_bus_add_device(LCD_SPI_HOST, &dev_cfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 device add failed: %s", esp_err_to_name(err));
        spi_bus_free(LCD_SPI_HOST);
        return err;
    }

    // Hardware reset, then the panel bring-up sequence.
    gpio_set_level(BOARD_LCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_LCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    lcd_cmd(ST_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(ST_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120)); // datasheet exit-sleep time
    uint8_t colmod = 0x55; // 16bpp
    lcd_cmd(ST_CMD_COLMOD, &colmod, 1);
    uint8_t madctl = 0x00; // portrait, RGB order
    lcd_cmd(ST_CMD_MADCTL, &madctl, 1);
    lcd_cmd(ST_CMD_INVON, NULL, 0); // this panel inverts
    lcd_cmd(ST_CMD_NORON, NULL, 0);
    lcd_cmd(ST_CMD_DISPON, NULL, 0);

    ESP_LOGI(TAG, "ST7789 %dx%d ready (offset %d,%d, inverted)",
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
    if (!s_spi || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        x + w > BOARD_LCD_WIDTH || y + h > BOARD_LCD_HEIGHT)
        return;

    static uint8_t line[LCD_MAX_XFER];
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    for (int i = 0; i < w; i++) {
        line[2 * i] = hi;
        line[2 * i + 1] = lo;
    }
    lcd_set_window(x, y, w, h);
    lcd_stream(line, w * 2, h);
}

void display_st7789_set_pixel(int x, int y, uint16_t color)
{
    if (!s_spi || x < 0 || y < 0 ||
        x >= BOARD_LCD_WIDTH || y >= BOARD_LCD_HEIGHT)
        return;

    uint8_t px[2] = { (uint8_t)(color >> 8), (uint8_t)(color & 0xFF) };
    lcd_set_window(x, y, 1, 1);
    lcd_stream(px, 2, 1);
}

void display_st7789_backlight(bool on)
{
    gpio_set_level(BOARD_LCD_BL_PIN, on ? 1 : 0);
}

#endif // CONFIG_NUCULA_BOARD_M5STICK
