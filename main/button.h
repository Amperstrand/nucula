#pragma once

#include "sdkconfig.h"

#if CONFIG_NUCULA_BOARD_M5STICK

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_NONE = 0,
    BTN_A, // front button, G37
    BTN_B, // side button, G39
} button_id_t;

bool button_init(void);
// Non-blocking; call repeatedly — a button reports as held once it has
// been down for the debounce interval.
button_id_t button_poll(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_NUCULA_BOARD_M5STICK
