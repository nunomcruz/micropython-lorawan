#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "timer.h"

#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// LoRaMAC-node uses a sorted linked list of TimerEvent_t objects.  One single
// esp_timer drives the list head; when it fires, TimerIrqHandler processes
// expired entries and re-arms the timer for the next head.
//
// Accuracy requirement: RX1 opens 1 s after TX, RX2 opens 2 s after TX.
// esp_timer with ESP_TIMER_TASK dispatch achieves < 1 ms jitter in practice,
// which is well within the LoRaWAN spec tolerance.

// ---- internal state ---------------------------------------------------------

static esp_timer_handle_t s_hw_timer   = NULL;
static TimerEvent_t      *s_list_head  = NULL;
static portMUX_TYPE       s_timer_mux  = portMUX_INITIALIZER_UNLOCKED;
static int64_t            s_start_us   = 0;  // esp_timer_get_time() when hw timer was armed

// ---- forward declarations ---------------------------------------------------

static void   timer_insert_new_head(TimerEvent_t *obj, uint32_t remaining_ms);
static void   timer_insert(TimerEvent_t *obj, uint32_t remaining_ms);
static void   timer_arm(TimerEvent_t *obj);
static bool   timer_exists(TimerEvent_t *obj);
static uint32_t hw_elapsed_ms(void);

// ---- hardware timer callback (runs in esp_timer task) -----------------------

static void hw_timer_cb(void *arg) {
    TimerIrqHandler();
}

static void hw_timer_ensure_created(void) {
    if (s_hw_timer) return;
    esp_timer_create_args_t args = {
        .callback        = hw_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "lorawan",
    };
    esp_timer_create(&args, &s_hw_timer);
}

// ---- public API -------------------------------------------------------------

void TimerInit(TimerEvent_t *obj, void (*callback)(void *context)) {
    if (!obj) return;
    memset(obj, 0, sizeof(TimerEvent_t));
    obj->Callback = callback;
    hw_timer_ensure_created();
}

void TimerSetContext(TimerEvent_t *obj, void *context) {
    if (obj) obj->Context = context;
}

void TimerStart(TimerEvent_t *obj) {
    if (!obj) return;

    taskENTER_CRITICAL(&s_timer_mux);

    if (timer_exists(obj)) {
        taskEXIT_CRITICAL(&s_timer_mux);
        return;
    }

    obj->Timestamp     = obj->ReloadValue;
    obj->IsStarted     = true;
    obj->IsNext2Expire = false;

    if (!s_list_head) {
        timer_insert_new_head(obj, obj->Timestamp);
    } else {
        // Compute how much time remains on the current head.
        uint32_t remaining;
        if (s_list_head->IsNext2Expire) {
            uint32_t elapsed = hw_elapsed_ms();
            remaining = (elapsed < s_list_head->Timestamp)
                        ? (s_list_head->Timestamp - elapsed)
                        : 0;
        } else {
            remaining = s_list_head->Timestamp;
        }

        if (obj->Timestamp < remaining) {
            timer_insert_new_head(obj, remaining);
        } else {
            timer_insert(obj, remaining);
        }
    }

    taskEXIT_CRITICAL(&s_timer_mux);
}

bool TimerIsStarted(TimerEvent_t *obj) {
    return obj ? obj->IsStarted : false;
}

void TimerStop(TimerEvent_t *obj) {
    if (!obj) return;

    taskENTER_CRITICAL(&s_timer_mux);

    if (!s_list_head) {
        taskEXIT_CRITICAL(&s_timer_mux);
        return;
    }

    if (s_list_head == obj) {
        if (obj->IsNext2Expire) {
            // The hw timer is counting for this object — stop it and transfer
            // the remaining time to the next entry.
            uint32_t elapsed   = hw_elapsed_ms();
            uint32_t remaining = (elapsed < obj->Timestamp)
                                 ? (obj->Timestamp - elapsed)
                                 : 0;
            esp_timer_stop(s_hw_timer);
            s_list_head = obj->Next;
            if (s_list_head) {
                s_list_head->Timestamp += remaining;
                s_list_head->IsNext2Expire = true;
                timer_arm(s_list_head);
            }
        } else {
            uint32_t remaining = obj->Timestamp;
            s_list_head = obj->Next;
            if (s_list_head) {
                s_list_head->Timestamp += remaining;
            }
        }
    } else {
        TimerEvent_t *prev = s_list_head;
        TimerEvent_t *cur  = s_list_head->Next;
        while (cur) {
            if (cur == obj) {
                prev->Next = cur->Next;
                if (cur->Next) {
                    cur->Next->Timestamp += cur->Timestamp;
                }
                break;
            }
            prev = cur;
            cur  = cur->Next;
        }
    }

    obj->IsStarted     = false;
    obj->IsNext2Expire = false;
    obj->Next          = NULL;

    taskEXIT_CRITICAL(&s_timer_mux);
}

void TimerReset(TimerEvent_t *obj) {
    TimerStop(obj);
    TimerStart(obj);
}

void TimerSetValue(TimerEvent_t *obj, uint32_t value) {
    if (!obj) return;
    TimerStop(obj);
    obj->Timestamp   = value;
    obj->ReloadValue = value;
}

TimerTime_t TimerGetCurrentTime(void) {
    return (TimerTime_t)(esp_timer_get_time() / 1000);
}

TimerTime_t TimerGetElapsedTime(TimerTime_t past) {
    if (past == 0) return 0;
    // uint32_t subtraction wraps correctly on rollover (~49 days uptime)
    return (TimerTime_t)(TimerGetCurrentTime() - past);
}

TimerTime_t TimerTempCompensation(TimerTime_t period, float temperature) {
    // Crystal oscillator correction not needed on ESP32 (TCXO or good crystal).
    (void)temperature;
    return period;
}

// TimerIrqHandler is invoked by hw_timer_cb from the esp_timer task.
// It fires all expired entries and re-arms the hw timer for the next one.
// Callbacks are called outside the critical section so they can themselves
// call TimerStart / TimerStop without deadlocking.
void TimerIrqHandler(void) {
    if (!s_list_head) return;

    taskENTER_CRITICAL(&s_timer_mux);

    // Adjust head by actual elapsed time (timer may fire slightly late).
    uint32_t elapsed = hw_elapsed_ms();
    s_list_head->IsNext2Expire = false;
    if (elapsed >= s_list_head->Timestamp) {
        s_list_head->Timestamp = 0;
    } else {
        s_list_head->Timestamp -= elapsed;
    }

    // Drain all entries that have reached zero.
    while (s_list_head && s_list_head->Timestamp == 0) {
        TimerEvent_t *expired  = s_list_head;
        s_list_head            = s_list_head->Next;
        expired->IsStarted     = false;
        expired->IsNext2Expire = false;
        expired->Next          = NULL;

        // Release the spinlock before calling the callback: the callback may
        // itself call TimerStart / TimerStop, which also take the lock.
        taskEXIT_CRITICAL(&s_timer_mux);
        if (expired->Callback) {
            expired->Callback(expired->Context);
        }
        taskENTER_CRITICAL(&s_timer_mux);
    }

    // Arm the hw timer for the next entry if one exists.
    if (s_list_head && !s_list_head->IsNext2Expire) {
        s_list_head->IsNext2Expire = true;
        timer_arm(s_list_head);
    }

    taskEXIT_CRITICAL(&s_timer_mux);
}

// TimerProcess is a no-op here: all processing happens in the esp_timer task.
void TimerProcess(void) {
}

// ---- internal helpers -------------------------------------------------------

// Returns milliseconds elapsed since the hw timer was last armed.
static uint32_t hw_elapsed_ms(void) {
    int64_t delta = esp_timer_get_time() - s_start_us;
    if (delta < 0) delta = 0;
    return (uint32_t)(delta / 1000);
}

// Start the hardware timer for obj->Timestamp milliseconds.
// Must be called with s_timer_mux held.
static void timer_arm(TimerEvent_t *obj) {
    s_start_us = esp_timer_get_time();
    esp_timer_stop(s_hw_timer);            // harmless if not running
    uint64_t us = (uint64_t)obj->Timestamp * 1000;
    if (us == 0) us = 1;                   // never arm for exactly 0 µs
    esp_timer_start_once(s_hw_timer, us);
}

// Insert obj as the new head of the sorted list.
// remaining_ms is the time left on the current head (before this call).
static void timer_insert_new_head(TimerEvent_t *obj, uint32_t remaining_ms) {
    TimerEvent_t *cur = s_list_head;

    if (cur) {
        // The old head's remaining time becomes relative to the new head.
        cur->Timestamp     = remaining_ms - obj->Timestamp;
        cur->IsNext2Expire = false;
    }

    obj->Next          = cur;
    obj->IsNext2Expire = true;
    s_list_head        = obj;
    timer_arm(s_list_head);
}

// Insert obj into the sorted list after the head.
// remaining_ms is the time left on the current head.
static void timer_insert(TimerEvent_t *obj, uint32_t remaining_ms) {
    uint32_t aggr      = remaining_ms;
    uint32_t aggr_next = remaining_ms;

    TimerEvent_t *prev = s_list_head;
    TimerEvent_t *cur  = s_list_head->Next;

    if (!cur) {
        obj->Timestamp -= remaining_ms;
        prev->Next      = obj;
        obj->Next       = NULL;
        return;
    }

    aggr_next = remaining_ms + cur->Timestamp;

    while (prev) {
        if (aggr_next > obj->Timestamp) {
            obj->Timestamp -= aggr;
            if (cur) cur->Timestamp -= obj->Timestamp;
            prev->Next = obj;
            obj->Next  = cur;
            return;
        }

        prev = cur;
        cur  = cur->Next;

        if (!cur) {
            aggr           = aggr_next;
            obj->Timestamp -= aggr;
            prev->Next      = obj;
            obj->Next       = NULL;
            return;
        }

        aggr       = aggr_next;
        aggr_next += cur->Timestamp;
    }
}

// Check whether obj is already in the list.
static bool timer_exists(TimerEvent_t *obj) {
    TimerEvent_t *cur = s_list_head;
    while (cur) {
        if (cur == obj) return true;
        cur = cur->Next;
    }
    return false;
}
