// AXP192 PMU driver for the M5StickC Plus. Register values follow the
// official M5Stack M5StickC-Plus AXP192 driver:
//   reg 0x28 = 0xCC  LDO2 (backlight) + LDO3 (LCD logic) at 3.0 V
//   reg 0x12 |= 0x4D DC-DC1 + LDO2 + LDO3 + EXTEN on
//   reg 0x10 bit 2   EXTEN boost — feeds the Grove port the MFRC522
//                    draws its power from; clearing it kills the reader

#include "axp192.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "board.h"

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

#define TAG "axp192"

#define AXP_REG_LDO23_VOLTAGE 0x28
#define AXP_REG_POWER_OUTPUT  0x12
#define AXP_REG_EXTEN_DCDC2   0x10
#define AXP_OUTPUT_RAILS      0x4D // DCDC1 + LDO2 + LDO3 + EXTEN
#define AXP_OUTPUT_LDO2       0x04
#define AXP_EXTEN_BIT         0x04

static i2c_master_dev_handle_t s_dev;

static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 100);
}

static esp_err_t axp_rmw(uint8_t reg, uint8_t mask, bool set)
{
    uint8_t cur;
    esp_err_t err = axp_read(reg, &cur);
    if (err != ESP_OK)
        return err;
    uint8_t want = set ? (uint8_t)(cur | mask) : (uint8_t)(cur & ~mask);
    if (want == cur)
        return ESP_OK;
    return axp_write(reg, want);
}

esp_err_t axp192_init(void)
{
    if (s_dev)
        return ESP_OK;

    // The PMU sits on its own I2C1 bus (G21/G22), separate from the
    // shared Grove bus the RC522 uses.
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

    // A probe NACK here means no AXP192: the Plus2 variant. Leave the
    // bus in place — nothing else claims I2C1 — so a later retry can
    // still succeed after the rails settle.
    if (i2c_master_probe(bus, BOARD_PMU_ADDR, 50) != ESP_OK) {
        i2c_del_master_bus(bus);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_PMU_ADDR,
        .scl_speed_hz    = 400000,
    };
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        i2c_del_master_bus(bus);
        return err;
    }

    err = axp_write(AXP_REG_LDO23_VOLTAGE, 0xCC);
    if (err == ESP_OK)
        err = axp_rmw(AXP_REG_POWER_OUTPUT, AXP_OUTPUT_RAILS, true);
    if (err == ESP_OK)
        err = axp_rmw(AXP_REG_EXTEN_DCDC2, AXP_EXTEN_BIT, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PMU rail sequencing failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "AXP192 ready (DCDC1+LDO2+LDO3+EXTEN on)");
    return ESP_OK;
}

esp_err_t axp192_backlight(bool on)
{
    if (!s_dev)
        return ESP_ERR_INVALID_STATE;
    return axp_rmw(AXP_REG_POWER_OUTPUT, AXP_OUTPUT_LDO2, on);
}

esp_err_t axp192_grove_power(bool on)
{
    if (!s_dev)
        return ESP_ERR_INVALID_STATE;
    return axp_rmw(AXP_REG_EXTEN_DCDC2, AXP_EXTEN_BIT, on);
}

#endif // CONFIG_NUCULA_BOARD_M5STICK
