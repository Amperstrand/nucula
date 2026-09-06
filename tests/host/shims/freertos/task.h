#pragma once
// Host shim: see freertos/FreeRTOS.h.
static inline void vTaskDelay(int ticks)
{
    (void)ticks;
}
