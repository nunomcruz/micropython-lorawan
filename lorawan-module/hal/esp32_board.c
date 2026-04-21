#include <stdint.h>
#include <string.h>
#include "board.h"
#include "utilities.h"
#include "rtc-board.h"

void BoardInitMcu(void)
{
}

void BoardResetMcu(void)
{
}

void BoardInitPeriph(void)
{
}

void BoardDeInitMcu(void)
{
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
    return 255;
}

int16_t BoardGetTemperature(void)
{
    return 25;
}

uint32_t BoardGetRandomSeed(void)
{
    return 0;
}

void BoardGetUniqueId(uint8_t *id)
{
    if (id) {
        memset(id, 0, 8);
    }
}

void BoardLowPowerHandler(void)
{
}

uint8_t GetBoardPowerSource(void)
{
    return USB_POWER;
}

void BoardCriticalSectionBegin(uint32_t *mask)
{
    (void)mask;
}

void BoardCriticalSectionEnd(uint32_t *mask)
{
    (void)mask;
}

/* RTC stubs — used by systime.c; real implementation in Phase 3 */

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

uint32_t RtcGetCalendarTime(uint16_t *ms)
{
    if (ms) *ms = 0;
    return 0;
}

uint32_t RtcGetTimerValue(void) { return 0; }

uint32_t RtcGetTimerElapsedTime(void) { return 0; }

void RtcBkupWrite(uint32_t data0, uint32_t data1)
{
    (void)data0; (void)data1;
}

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
