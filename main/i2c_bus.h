#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Create the shared I2C master bus (pins from board.h). Idempotent.
esp_err_t i2c_bus_init(void);

// Handle of the shared bus, or NULL if i2c_bus_init failed / wasn't called.
i2c_master_bus_handle_t i2c_bus_get(void);

// AN10217 stuck-bus recovery: clock SCL up to `max_clocks` times until the
// slave releases SDA, then a STOP. Returns true when SDA reads high after.
// Steals the pins from any live I2C driver — reboot before using the bus again.
bool i2c_bus_recover(int max_clocks);

#ifdef __cplusplus
}
#endif
