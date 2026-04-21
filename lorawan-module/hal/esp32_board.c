#include <stdint.h>
#include <string.h>

#include "board.h"
#include "utilities.h"
#include "rtc-board.h"
#include "lorawan_config.h"
#include "gpio.h"
#include "spi.h"

#include "esp_system.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static bool mcu_initialized = false;

void BoardInitMcu(void)
{
    if (mcu_initialized) return;
    mcu_initialized = true;
}

void BoardResetMcu(void)
{
    esp_restart();
}

void BoardInitPeriph(void)
{
    // SPI bus and radio pin init is deferred to SX1276IoInit / SX126xIoInit
    // which are called by the radio driver on first use.
}

void BoardDeInitMcu(void)
{
    mcu_initialized = false;
}

uint8_t BoardGetPotiLevel(void)
{
    return 0;
}

uint32_t BoardGetBatteryVoltage(void)
{
    return 3300;
}

uint8_t BoardGetBatteryLevel(void)
{
    // 255 = unknown / not implemented
    // AXP PMU reads require I2C and knowledge of which PMU is present;
    // this is deferred until the Python bindings layer initialises the PMU.
    return 255;
}

int16_t BoardGetTemperature(void)
{
    return 25;
}

uint32_t BoardGetRandomSeed(void)
{
    return esp_random();
}

void BoardGetUniqueId(uint8_t *id)
{
    if (!id) return;

    // Use the 6-byte WiFi MAC as a base; pad to 8 bytes with fixed magic.
    uint8_t mac[6];
    memset(id, 0, 8);
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        // Bytes 0-5: MAC, bytes 6-7: distinguisher so both halves differ
        memcpy(id, mac, 6);
        id[6] = 0xFE;
        id[7] = 0xFF;
    }
}

void BoardLowPowerHandler(void)
{
    // No deep-sleep support in this HAL revision.
}

uint8_t GetBoardPowerSource(void)
{
    return USB_POWER;
}

Version_t BoardGetVersion(void)
{
    Version_t v = { .Value = 0x01000000 };  // v1.0.0.0
    return v;
}

void BoardCriticalSectionBegin(uint32_t *mask)
{
    (void)mask;
    taskDISABLE_INTERRUPTS();
}

void BoardCriticalSectionEnd(uint32_t *mask)
{
    (void)mask;
    taskENABLE_INTERRUPTS();
}

/* RTC stubs — used by systime.c; real implementation deferred. */

void RtcInit(void) {}
uint32_t RtcGetMinimumTimeout(void) { return 1; }
uint32_t RtcMs2Tick(TimerTime_t ms) { return ms; }
TimerTime_t RtcTick2Ms(uint32_t tick) { return (TimerTime_t)tick; }
void RtcDelayMs(TimerTime_t ms) { (void)ms; }
void RtcSetAlarm(uint32_t timeout) { (void)timeout; }
void RtcStopAlarm(void) {}
void RtcStartAlarm(uint32_t timeout) { (void)timeout; }
uint32_t RtcSetTimerContext(void) { return 0; }
uint32_t RtcGetTimerContext(void) { return 0; }
uint32_t RtcGetCalendarTime(uint16_t *ms) { if (ms) *ms = 0; return 0; }
uint32_t RtcGetTimerValue(void) { return 0; }
uint32_t RtcGetTimerElapsedTime(void) { return 0; }
void RtcBkupWrite(uint32_t data0, uint32_t data1) { (void)data0; (void)data1; }
void RtcBkupRead(uint32_t *data0, uint32_t *data1)
{
    if (data0) *data0 = 0;
    if (data1) *data1 = 0;
}
void RtcProcess(void) {}
TimerTime_t RtcTempCompensation(TimerTime_t period, float temperature)
{
    (void)temperature;
    return period;
}
