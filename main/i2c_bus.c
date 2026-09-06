#include "i2c_bus.h"
#include "board.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"

#define TAG "i2c_bus"

static i2c_master_bus_handle_t s_bus;

// Full stuck-bus procedure (NXP AN10217 "bus clear", TI SLVA704):
// clock SCL until the slave releases SDA (cap 32 clocks — a multi-byte
// burst can need more than the classic 9), then a STOP so the slave's
// state machine returns to idle. A slave interrupted mid-transaction —
// e.g. an MFRC522 left hanging by a prior firmware that watchdog-reset
// mid-transfer — holds SDA low and will ACK nothing until recovered.
// Must run BEFORE the I2C driver claims the pins. Returns true when the
// bus is usable afterwards (idle or recovered); false means an
// electrical fault (SDA shorted or slave latch-up) — only removing
// reader power clears that.
static bool recover_i2c_bus_n(int scl_pin, int sda_pin, int max_clocks)
{
    gpio_config_t sda_in = {
        .pin_bit_mask = 1ULL << sda_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sda_in);
    if (gpio_get_level(sda_pin) != 0) {
        ESP_LOGD(TAG, "recovery: SDA high, bus OK");
        gpio_reset_pin(sda_pin);
        return true;
    }

    ESP_LOGW(TAG, "recovery: SDA stuck LOW — clocking SCL (max %d)", max_clocks);
    gpio_config_t scl_out = {
        .pin_bit_mask = 1ULL << scl_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (gpio_config(&scl_out) != ESP_OK) {
        gpio_reset_pin(sda_pin);
        return false;
    }

    int clocks = 0;
    while (clocks < max_clocks) {
        gpio_set_level(scl_pin, 1);
        esp_rom_delay_us(10);
        clocks++;
        if (gpio_get_level(sda_pin) != 0)
            break;
        gpio_set_level(scl_pin, 0);
        esp_rom_delay_us(10);
    }

    bool released = gpio_get_level(sda_pin) != 0;

    // STOP: with SCL high, SDA transitions low -> high.
    gpio_config_t sda_out = {
        .pin_bit_mask = 1ULL << sda_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sda_out);
    gpio_set_level(scl_pin, 0);
    esp_rom_delay_us(10);
    gpio_set_level(sda_pin, 0);
    esp_rom_delay_us(10);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(10);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(10);

    bool ok = released && gpio_get_level(sda_pin) != 0 &&
              gpio_get_level(scl_pin) != 0;
    gpio_reset_pin(scl_pin);
    gpio_reset_pin(sda_pin);

    if (ok)
        ESP_LOGI(TAG, "recovery: SDA released after %d clocks + STOP", clocks);
    else
        ESP_LOGE(TAG, "recovery: SDA still LOW — electrical fault "
                      "(short or latch-up); power-cycle the reader");
    return ok;
}

esp_err_t i2c_bus_init(void)
{
    if (s_bus)
        return ESP_OK;

    recover_i2c_bus_n(BOARD_I2C_SCL_PIN, BOARD_I2C_SDA_PIN, 32);

    i2c_master_bus_config_t cfg = {
        .i2c_port          = I2C_NUM_0,
        .sda_io_num        = BOARD_I2C_SDA_PIN,
        .scl_io_num        = BOARD_I2C_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus create failed: %s", esp_err_to_name(err));
        s_bus = NULL;
    }
    return err;
}

i2c_master_bus_handle_t i2c_bus_get(void)
{
    return s_bus;
}

bool i2c_bus_recover(int max_clocks)
{
    return recover_i2c_bus_n(BOARD_I2C_SCL_PIN, BOARD_I2C_SDA_PIN, max_clocks);
}
