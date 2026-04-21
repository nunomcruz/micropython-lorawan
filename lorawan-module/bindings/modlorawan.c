#include "py/runtime.h"
#include "py/obj.h"

#include "radio_select.h"
#include "sx1276-board.h"
#include "sx1276/sx1276.h"
#include "timer.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile bool s_test_timer_fired = false;

static void test_timer_cb(void *ctx) {
    (void)ctx;
    s_test_timer_fired = true;
}

static mp_obj_t lorawan_version(void) {
    return mp_obj_new_str("0.1.0", 5);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lorawan_version_obj, lorawan_version);

// lorawan.test_hal() -- exercises the ESP32 HAL without starting the MAC stack.
//
// On SX1276 hardware: reads register 0x42 (version reg), should return 0x12.
// On SX1262 hardware: reg 0x42 is not the version register; sx1276 key is False.
// Timer test: starts a 200 ms one-shot timer and verifies the callback fires.
// Selects the Radio driver matching the detected hardware before returning.
//
// Returns: {"reg_42": <int>, "sx1276": <bool>, "timer_ok": <bool>}
static mp_obj_t lorawan_test_hal(void) {
    // Step 1: init SPI bus and radio GPIO (NSS, RESET, DIO0/1)
    SX1276IoInit();

    // Step 2: hard-reset the radio to a known state
    SX1276Reset();

    // Step 3: read register 0x42 — SX1276 version register returns 0x12
    uint8_t reg42 = SX1276Read(0x42);
    bool is_sx1276 = (reg42 == 0x12);

    mp_printf(&mp_plat_print,
              "lorawan.test_hal: reg 0x42 = 0x%02X (%s)\n",
              reg42,
              is_sx1276 ? "SX1276 OK" : "not SX1276 — may be SX1262");

    // Step 4: point LoRaMAC-node Radio at the right driver
    lorawan_radio_select(is_sx1276);

    // Step 5: 200 ms timer test
    static TimerEvent_t s_test_timer;
    s_test_timer_fired = false;
    TimerInit(&s_test_timer, test_timer_cb);
    TimerSetValue(&s_test_timer, 200);
    TimerStart(&s_test_timer);

    // Yield to FreeRTOS for up to 500 ms so the esp_timer task can fire
    int64_t deadline = esp_timer_get_time() + 500000LL;
    while (!s_test_timer_fired && esp_timer_get_time() < deadline) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    bool timer_ok = s_test_timer_fired;

    mp_printf(&mp_plat_print,
              "lorawan.test_hal: 200ms timer = %s\n",
              timer_ok ? "OK" : "FAIL (did not fire within 500ms)");

    // Step 6: build and return result dict
    mp_obj_t d = mp_obj_new_dict(0);
    mp_obj_dict_store(d, mp_obj_new_str("reg_42",   6), mp_obj_new_int(reg42));
    mp_obj_dict_store(d, mp_obj_new_str("sx1276",   6), mp_obj_new_bool(is_sx1276));
    mp_obj_dict_store(d, mp_obj_new_str("timer_ok", 8), mp_obj_new_bool(timer_ok));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lorawan_test_hal_obj, lorawan_test_hal);

static const mp_rom_map_elem_t lorawan_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_lorawan) },
    { MP_ROM_QSTR(MP_QSTR_version),   MP_ROM_PTR(&lorawan_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_test_hal),  MP_ROM_PTR(&lorawan_test_hal_obj) },
};
static MP_DEFINE_CONST_DICT(lorawan_module_globals, lorawan_module_globals_table);

const mp_obj_module_t lorawan_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lorawan_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lorawan, lorawan_module);
