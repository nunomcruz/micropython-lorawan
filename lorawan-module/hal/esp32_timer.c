#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "timer.h"

void TimerInit(TimerEvent_t *obj, void (*callback)(void *context))
{
    if (obj) {
        memset(obj, 0, sizeof(TimerEvent_t));
        obj->Callback = callback;
    }
}

void TimerSetContext(TimerEvent_t *obj, void *context)
{
    if (obj) {
        obj->Context = context;
    }
}

void TimerIrqHandler(void)
{
}

void TimerStart(TimerEvent_t *obj)
{
    if (obj) {
        obj->IsStarted = true;
    }
}

bool TimerIsStarted(TimerEvent_t *obj)
{
    return obj ? obj->IsStarted : false;
}

void TimerStop(TimerEvent_t *obj)
{
    if (obj) {
        obj->IsStarted = false;
    }
}

void TimerReset(TimerEvent_t *obj)
{
    TimerStop(obj);
    TimerStart(obj);
}

void TimerSetValue(TimerEvent_t *obj, uint32_t value)
{
    if (obj) {
        obj->ReloadValue = value;
    }
}

TimerTime_t TimerGetCurrentTime(void)
{
    return 0;
}

TimerTime_t TimerGetElapsedTime(TimerTime_t past)
{
    (void)past;
    return 0;
}

TimerTime_t TimerTempCompensation(TimerTime_t period, float temperature)
{
    (void)temperature;
    return period;
}

void TimerProcess(void)
{
}
