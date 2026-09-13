// M5StickC Plus buttons: front (A, G37) and side (B, G39), active low.
// G37/G39 are input-only pads without internal pull-ups; the board's
// external pull-ups do the work, so the pull-up request here is a no-op
// that documents the idle level.

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include "button.h"
#include "board.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

#define TAG "button"

#define DEBOUNCE_US 20000 // 20 ms stable-low confirms a press

static bool s_init = false;

bool button_init(void)
{
    const gpio_num_t pins[] = { BOARD_BTN_A_PIN, BOARD_BTN_B_PIN };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
        };
        if (gpio_config(&io) != ESP_OK) {
            ESP_LOGE(TAG, "gpio %d config failed", pins[i]);
            return false;
        }
    }
    s_init = true;
    return true;
}

button_id_t button_poll(void)
{
    static int64_t low_since[2] = { -1, -1 };
    if (!s_init)
        return BTN_NONE;

    const gpio_num_t pins[] = { BOARD_BTN_A_PIN, BOARD_BTN_B_PIN };
    for (int i = 0; i < 2; i++) {
        if (gpio_get_level(pins[i]) != 0) {
            low_since[i] = -1;
            continue;
        }
        if (low_since[i] < 0)
            low_since[i] = esp_timer_get_time();
        if (esp_timer_get_time() - low_since[i] >= DEBOUNCE_US)
            return (button_id_t)(BTN_A + i);
    }
    return BTN_NONE;
}

#endif // CONFIG_NUCULA_BOARD_M5STICK
