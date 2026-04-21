#include <stdint.h>

#include "delay.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

// Short delays (≤ 10 ms) use a busy-wait so they don't surrender the CPU
// scheduler tick, which would introduce up to one full tick of jitter.
// Longer delays yield to the FreeRTOS scheduler via vTaskDelay.

void DelayMs(uint32_t ms) {
    if (ms == 0) return;

    if (ms <= 10) {
        esp_rom_delay_us(ms * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
}

void Delay(float s) {
    uint32_t ms = (uint32_t)(s * 1000.0f + 0.5f);
    DelayMs(ms);
}
