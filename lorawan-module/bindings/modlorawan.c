// modlorawan.c — LoRaWAN Python bindings
// Phase 4, Session 7: lorawan_obj_t, __init__, join_abp, send
// FreeRTOS task owns all MAC calls; Python thread communicates via queue + event group.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mperrno.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "radio_select.h"
#include "sx1276-board.h"
#include "sx126x-board.h"
#include "sx1276/sx1276.h"
#include "timer.h"
#include "board.h"
#include "lorawan_config.h"
#include "LoRaMac.h"
#include "region/RegionEU868.h"

// ---- Constants ----

// 8 KB: LoRaMacInitialization() → SX1276Init() → RxChainCalibration() is a
// deep call chain; 4 KB was tight with the 222-byte payload on the stack.
#define LORAWAN_TASK_STACK  8192
#define LORAWAN_TASK_PRIO   6
#define LORAWAN_CMD_QSIZE   8
#define LORAWAN_RX_QSIZE    4

// Max payload: DR5 EU868 = 222 bytes
#define LW_PAYLOAD_MAX  222

// Event group bits
#define EVT_COMPLETED   (1u << 0)
#define EVT_INIT_ERROR  (1u << 1)
#define EVT_TX_DONE     (1u << 2)
#define EVT_TX_ERROR    (1u << 3)

// ---- Command types ----

typedef enum {
    CMD_INIT = 0,
    CMD_JOIN_ABP,
    CMD_TX,
} lorawan_cmd_t;

typedef struct {
    uint32_t dev_addr;
    uint8_t  nwk_s_key[16];
    uint8_t  app_s_key[16];
} cmd_join_abp_t;

typedef struct {
    uint8_t  data[LW_PAYLOAD_MAX];
    uint8_t  len;
    uint8_t  port;
    bool     confirmed;
} cmd_tx_t;

typedef struct {
    lorawan_cmd_t cmd;
    union {
        cmd_join_abp_t join_abp;
        cmd_tx_t       tx;
    };
} lorawan_cmd_data_t;

// ---- Received packet ----

typedef struct {
    uint8_t  data[LW_PAYLOAD_MAX];
    uint8_t  len;
    uint8_t  port;
    int16_t  rssi;
    int8_t   snr;
} lorawan_rx_pkt_t;

// ---- Python object ----

typedef struct {
    mp_obj_base_t   base;
    bool            initialized;
    bool            joined;
    bool            is_sx1276;
    LoRaMacRegion_t region;
    DeviceClass_t   device_class;
    // Python callbacks (reserved for Session 9)
    mp_obj_t        rx_callback;
    mp_obj_t        tx_callback;
    // Last uplink stats
    uint32_t        tx_counter;
    uint32_t        tx_time_on_air;
    // Last downlink stats
    int16_t         rssi;
    int8_t          snr;
} lorawan_obj_t;

// ---- Static FreeRTOS state (one LoRaWAN instance per firmware) ----

static QueueHandle_t      s_cmd_queue;
static QueueHandle_t      s_rx_queue;
static EventGroupHandle_t s_events;
static TaskHandle_t       s_task_handle;

static LoRaMacPrimitives_t s_mac_primitives;
static LoRaMacCallback_t   s_mac_callbacks;

static lorawan_obj_t *s_lora_obj;       // weak ref — valid while the object lives
static volatile bool  s_mac_initialized; // set true after LoRaMacInitialization() succeeds

// ---- LoRaMAC MAC-layer callbacks (run in the LoRaWAN task context) ----

static void mcps_confirm(McpsConfirm_t *c) {
    if (!c) return;
    if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
        if (s_lora_obj) {
            s_lora_obj->tx_counter     = c->UpLinkCounter;
            s_lora_obj->tx_time_on_air = c->TxTimeOnAir;
        }
        xEventGroupSetBits(s_events, EVT_TX_DONE);
    } else {
        xEventGroupSetBits(s_events, EVT_TX_ERROR);
    }
}

static void mcps_indication(McpsIndication_t *ind) {
    if (!ind || ind->Status != LORAMAC_EVENT_INFO_STATUS_OK) return;
    if (!ind->RxData || ind->BufferSize == 0) return;
    if (ind->Port == 0 || ind->Port >= 224) return;

    if (s_lora_obj) {
        s_lora_obj->rssi = ind->Rssi;
        s_lora_obj->snr  = ind->Snr;
    }

    lorawan_rx_pkt_t pkt;
    pkt.len  = (ind->BufferSize <= LW_PAYLOAD_MAX) ? (uint8_t)ind->BufferSize : LW_PAYLOAD_MAX;
    pkt.port = ind->Port;
    pkt.rssi = ind->Rssi;
    pkt.snr  = ind->Snr;
    memcpy(pkt.data, ind->Buffer, pkt.len);
    xQueueSend(s_rx_queue, &pkt, 0);
}

static void mlme_confirm(MlmeConfirm_t *c) {
    // OTAA join confirmation — Session 8
    (void)c;
}

static void mlme_indication(MlmeIndication_t *ind) {
    (void)ind;
}

// Called by LoRaMAC to signal that LoRaMacProcess() must be called.
// May be invoked from a GPIO ISR (radio DIO) or the esp_timer task.
// Passing NULL for pxHigherPriorityTaskWoken skips the yield-from-ISR
// path and avoids portYIELD_FROM_ISR portability issues; the LoRaWAN
// task will wake on the next scheduler tick if it was blocked.
static IRAM_ATTR void on_mac_process_notify(void) {
    if (s_task_handle) {
        vTaskNotifyGiveFromISR(s_task_handle, NULL);
    }
}

// ---- LoRaWAN task ----

static void lorawan_task(void *arg) {
    (void)arg;

    MibRequestConfirm_t mib;
    McpsReq_t           mcps;
    lorawan_cmd_data_t  cmd;

    for (;;) {
        // Sleep until MacProcessNotify wakes us, or 2 ms passes.
        // pdMS_TO_TICKS(2) is correct on any tick rate; the old form
        // 2/portTICK_PERIOD_MS = 0 on a 100 Hz (10 ms) tick system,
        // which turned this into a busy-spin.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2));

        // Guard: LoRaMacProcess() must not be called before the MAC stack is
        // initialised.  The CMD_INIT handler sets s_mac_initialized after
        // LoRaMacInitialization() + LoRaMacStart() succeed.
        if (s_mac_initialized) {
            LoRaMacProcess();
        }

        while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.cmd) {

            case CMD_INIT: {
                BoardInitMcu();
                BoardInitPeriph();

                s_mac_primitives.MacMcpsConfirm    = mcps_confirm;
                s_mac_primitives.MacMcpsIndication = mcps_indication;
                s_mac_primitives.MacMlmeConfirm    = mlme_confirm;
                s_mac_primitives.MacMlmeIndication = mlme_indication;

                s_mac_callbacks.GetBatteryLevel    = BoardGetBatteryLevel;
                s_mac_callbacks.GetTemperatureLevel = NULL;
                s_mac_callbacks.NvmDataChange       = NULL;
                s_mac_callbacks.MacProcessNotify    = on_mac_process_notify;

                LoRaMacRegion_t region =
                    s_lora_obj ? s_lora_obj->region : LORAMAC_REGION_EU868;

                // esp_rom_printf is GIL-free; mp_printf from this task deadlocks
                // because the Python thread holds the GIL while sleeping in
                // xEventGroupWaitBits.
                esp_rom_printf("lorawan: LoRaMacInitialization start\n");
                LoRaMacStatus_t st =
                    LoRaMacInitialization(&s_mac_primitives, &s_mac_callbacks, region);
                esp_rom_printf("lorawan: LoRaMacInitialization returned %d\n", (int)st);

                if (st != LORAMAC_STATUS_OK) {
                    xEventGroupSetBits(s_events, EVT_INIT_ERROR);
                    break;
                }

                LoRaMacStart();

                // ADR off: fixed DR5 (SF7/BW125) for reliable first uplinks
                mib.Type = MIB_ADR;
                mib.Param.AdrEnable = false;
                LoRaMacMibSetRequestConfirm(&mib);

                // Public network sync word (TTN)
                mib.Type = MIB_PUBLIC_NETWORK;
                mib.Param.EnablePublicNetwork = true;
                LoRaMacMibSetRequestConfirm(&mib);

                // Class A
                DeviceClass_t cls =
                    s_lora_obj ? s_lora_obj->device_class : CLASS_A;
                mib.Type = MIB_DEVICE_CLASS;
                mib.Param.Class = cls;
                LoRaMacMibSetRequestConfirm(&mib);

                // EU868 RX2: 869.525 MHz, DR3 (SF9/BW125) — TTN default
                // The LoRaMAC-node default is DR0; wrong value = missed downlinks.
                mib.Type = MIB_RX2_CHANNEL;
                mib.Param.Rx2Channel.Frequency = 869525000;
                mib.Param.Rx2Channel.Datarate  = DR_3;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_RX2_DEFAULT_CHANNEL;
                mib.Param.Rx2DefaultChannel.Frequency = 869525000;
                mib.Param.Rx2DefaultChannel.Datarate  = DR_3;
                LoRaMacMibSetRequestConfirm(&mib);

                s_mac_initialized = true;
                if (s_lora_obj) s_lora_obj->initialized = true;
                esp_rom_printf("lorawan: init done\n");
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_JOIN_ABP: {
                // LoRaWAN 1.0 ABP: the single NwkSKey maps to all three
                // v1.1 network session keys. AppSKey is set separately.
                uint8_t *nwk = cmd.join_abp.nwk_s_key;
                uint8_t *app = cmd.join_abp.app_s_key;

                mib.Type = MIB_NETWORK_ACTIVATION;
                mib.Param.NetworkActivation = ACTIVATION_TYPE_ABP;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_NET_ID;
                mib.Param.NetID = 0;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_DEV_ADDR;
                mib.Param.DevAddr = cmd.join_abp.dev_addr;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_F_NWK_S_INT_KEY;
                mib.Param.FNwkSIntKey = nwk;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_S_NWK_S_INT_KEY;
                mib.Param.SNwkSIntKey = nwk;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_NWK_S_ENC_KEY;
                mib.Param.NwkSEncKey = nwk;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_APP_S_KEY;
                mib.Param.AppSKey = app;
                LoRaMacMibSetRequestConfirm(&mib);

                if (s_lora_obj) s_lora_obj->joined = true;
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_TX: {
                LoRaMacTxInfo_t tx_info;

                if (LoRaMacQueryTxPossible(cmd.tx.len, &tx_info) != LORAMAC_STATUS_OK) {
                    // Flush pending MAC commands with an empty unconfirmed frame
                    mcps.Type = MCPS_UNCONFIRMED;
                    mcps.Req.Unconfirmed.fBuffer     = NULL;
                    mcps.Req.Unconfirmed.fBufferSize = 0;
                    mcps.Req.Unconfirmed.Datarate    = DR_5;
                    LoRaMacMcpsRequest(&mcps);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                    break;
                }

                if (cmd.tx.confirmed) {
                    mcps.Type = MCPS_CONFIRMED;
                    mcps.Req.Confirmed.fPort       = cmd.tx.port;
                    mcps.Req.Confirmed.fBuffer     = cmd.tx.data;
                    mcps.Req.Confirmed.fBufferSize = cmd.tx.len;
                    mcps.Req.Confirmed.Datarate    = DR_5;
                } else {
                    mcps.Type = MCPS_UNCONFIRMED;
                    mcps.Req.Unconfirmed.fPort       = cmd.tx.port;
                    mcps.Req.Unconfirmed.fBuffer     = cmd.tx.data;
                    mcps.Req.Unconfirmed.fBufferSize = cmd.tx.len;
                    mcps.Req.Unconfirmed.Datarate    = DR_5;
                }

                LoRaMacStatus_t st = LoRaMacMcpsRequest(&mcps);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: McpsRequest failed: %d\n", (int)st);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                }
                // EVT_TX_DONE / EVT_TX_ERROR set asynchronously by mcps_confirm
                break;
            }

            default:
                break;
            }
        }
    }
}

// ---- Internal helper ----

static void send_cmd_wait(lorawan_cmd_data_t *cmd, EventBits_t wait_bits) {
    xEventGroupClearBits(s_events, wait_bits);
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    xTaskNotifyGive(s_task_handle);
    xEventGroupWaitBits(s_events, wait_bits, pdTRUE, pdFALSE, portMAX_DELAY);
}

// ---- Python type: lorawan.LoRaWAN ----

static mp_obj_t lorawan_make_new(const mp_obj_type_t *type,
                                  size_t n_args, size_t n_kw,
                                  const mp_obj_t *all_args) {
    enum { ARG_region, ARG_radio, ARG_device_class };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_region,       MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_radio,        MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_device_class, MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = CLASS_A} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
                               MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (s_task_handle != NULL) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("LoRaWAN already initialized; reset first"));
    }

    lorawan_obj_t *self = mp_obj_malloc(lorawan_obj_t, type);
    self->initialized    = false;
    self->joined         = false;
    self->is_sx1276      = true;
    self->region         = (LoRaMacRegion_t)args[ARG_region].u_int;
    self->device_class   = (DeviceClass_t)args[ARG_device_class].u_int;
    self->rx_callback    = mp_const_none;
    self->tx_callback    = mp_const_none;
    self->tx_counter     = 0;
    self->tx_time_on_air = 0;
    self->rssi           = 0;
    self->snr            = 0;

    // Radio detection: bring up SX1276 HAL (GPIO + SPI), reset, read reg 0x42.
    // SX1276 version register returns 0x12; anything else means SX1262.
    SX1276IoInit();
    SX1276Reset();
    uint8_t reg42 = SX1276Read(0x42);
    bool is_sx1276;

    if (args[ARG_radio].u_obj != mp_const_none) {
        // Caller forced a radio: radio=True → SX1276, radio=False → SX1262
        is_sx1276 = mp_obj_is_true(args[ARG_radio].u_obj);
    } else {
        is_sx1276 = (reg42 == 0x12);
    }

    if (!is_sx1276) {
        SX126xIoInit();
    }

    self->is_sx1276 = is_sx1276;
    lorawan_radio_select(is_sx1276);

    mp_printf(&mp_plat_print, "lorawan: radio=%s (reg42=0x%02X)\n",
              is_sx1276 ? "SX1276" : "SX1262", reg42);

    s_lora_obj  = self;
    s_cmd_queue = xQueueCreate(LORAWAN_CMD_QSIZE, sizeof(lorawan_cmd_data_t));
    s_rx_queue  = xQueueCreate(LORAWAN_RX_QSIZE,  sizeof(lorawan_rx_pkt_t));
    s_events    = xEventGroupCreate();

    BaseType_t task_ok = xTaskCreatePinnedToCore(lorawan_task, "lorawan",
                            LORAWAN_TASK_STACK / sizeof(StackType_t),
                            NULL, LORAWAN_TASK_PRIO,
                            &s_task_handle, 1);
    if (task_ok != pdPASS) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("failed to create LoRaWAN task (out of memory?)"));
    }

    lorawan_cmd_data_t cmd = { .cmd = CMD_INIT };
    send_cmd_wait(&cmd, EVT_COMPLETED | EVT_INIT_ERROR);

    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("LoRaMAC initialization failed"));
    }

    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t lorawan_join_abp(size_t n_args, const mp_obj_t *pos_args,
                                  mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);

    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }

    enum { ARG_dev_addr, ARG_nwk_s_key, ARG_app_s_key };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_dev_addr,  MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_nwk_s_key, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_app_s_key, MP_ARG_REQUIRED | MP_ARG_OBJ },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t nwk_buf, app_buf;
    mp_get_buffer_raise(args[ARG_nwk_s_key].u_obj, &nwk_buf, MP_BUFFER_READ);
    mp_get_buffer_raise(args[ARG_app_s_key].u_obj, &app_buf, MP_BUFFER_READ);

    if (nwk_buf.len != 16 || app_buf.len != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("keys must be 16 bytes"));
    }

    lorawan_cmd_data_t cmd;
    cmd.cmd               = CMD_JOIN_ABP;
    cmd.join_abp.dev_addr = (uint32_t)args[ARG_dev_addr].u_int;
    memcpy(cmd.join_abp.nwk_s_key, nwk_buf.buf, 16);
    memcpy(cmd.join_abp.app_s_key, app_buf.buf, 16);

    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_join_abp_obj, 1, lorawan_join_abp);

static mp_obj_t lorawan_send(size_t n_args, const mp_obj_t *pos_args,
                              mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);

    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (!self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not joined"));
    }

    enum { ARG_data, ARG_port, ARG_confirmed };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data,      MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_port,      MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int  = 1} },
        { MP_QSTR_confirmed, MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = false} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[ARG_data].u_obj, &buf, MP_BUFFER_READ);

    if (buf.len > LW_PAYLOAD_MAX) {
        mp_raise_ValueError(MP_ERROR_TEXT("payload too large"));
    }

    lorawan_cmd_data_t cmd;
    cmd.cmd          = CMD_TX;
    cmd.tx.len       = (uint8_t)buf.len;
    cmd.tx.port      = (uint8_t)args[ARG_port].u_int;
    cmd.tx.confirmed = args[ARG_confirmed].u_bool;
    memcpy(cmd.tx.data, buf.buf, buf.len);

    xEventGroupClearBits(s_events, EVT_TX_DONE | EVT_TX_ERROR);
    xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY);
    xTaskNotifyGive(s_task_handle);

    // Block up to 10 s for TX confirmation (covers RX1 + RX2 windows)
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           EVT_TX_DONE | EVT_TX_ERROR,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(10000));

    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("send failed"));
    }
    if (!(bits & EVT_TX_DONE)) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("send timeout"));
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_send_obj, 1, lorawan_send);

static mp_obj_t lorawan_joined_meth(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->joined);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_joined_obj, lorawan_joined_meth);

static mp_obj_t lorawan_stats(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t d = mp_obj_new_dict(0);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rssi),
                      mp_obj_new_int(self->rssi));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_snr),
                      mp_obj_new_int(self->snr));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_counter),
                      mp_obj_new_int(self->tx_counter));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_time_on_air),
                      mp_obj_new_int(self->tx_time_on_air));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_stats_obj, lorawan_stats);

static const mp_rom_map_elem_t lorawan_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_join_abp),  MP_ROM_PTR(&lorawan_join_abp_obj) },
    { MP_ROM_QSTR(MP_QSTR_send),      MP_ROM_PTR(&lorawan_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_joined),    MP_ROM_PTR(&lorawan_joined_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats),     MP_ROM_PTR(&lorawan_stats_obj) },
};
static MP_DEFINE_CONST_DICT(lorawan_locals, lorawan_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    lorawan_type,
    MP_QSTR_LoRaWAN,
    MP_TYPE_FLAG_NONE,
    make_new, lorawan_make_new,
    locals_dict, &lorawan_locals
);

// ---- Module-level functions ----

static mp_obj_t lorawan_version(void) {
    return mp_obj_new_str("0.2.0", 5);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lorawan_version_obj, lorawan_version);

// lorawan.test_hal() — preserved from Phase 3, exercises HAL without the MAC stack
static volatile bool s_test_timer_fired = false;

static void test_timer_cb(void *ctx) {
    (void)ctx;
    s_test_timer_fired = true;
}

static mp_obj_t lorawan_test_hal(void) {
    SX1276IoInit();
    SX1276Reset();
    uint8_t reg42     = SX1276Read(0x42);
    bool    is_sx1276 = (reg42 == 0x12);

    mp_printf(&mp_plat_print,
              "lorawan.test_hal: reg 0x42 = 0x%02X (%s)\n",
              reg42, is_sx1276 ? "SX1276 OK" : "not SX1276 — may be SX1262");

    lorawan_radio_select(is_sx1276);

    static TimerEvent_t s_test_timer;
    s_test_timer_fired = false;
    TimerInit(&s_test_timer, test_timer_cb);
    TimerSetValue(&s_test_timer, 200);
    TimerStart(&s_test_timer);

    int64_t deadline = esp_timer_get_time() + 500000LL;
    while (!s_test_timer_fired && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool timer_ok = s_test_timer_fired;

    mp_printf(&mp_plat_print,
              "lorawan.test_hal: 200ms timer = %s\n",
              timer_ok ? "OK" : "FAIL (did not fire within 500ms)");

    mp_obj_t d = mp_obj_new_dict(0);
    mp_obj_dict_store(d, mp_obj_new_str("reg_42",   6), mp_obj_new_int(reg42));
    mp_obj_dict_store(d, mp_obj_new_str("sx1276",   6), mp_obj_new_bool(is_sx1276));
    mp_obj_dict_store(d, mp_obj_new_str("timer_ok", 8), mp_obj_new_bool(timer_ok));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(lorawan_test_hal_obj, lorawan_test_hal);

// ---- Module definition ----

static const mp_rom_map_elem_t lorawan_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),  MP_ROM_QSTR(MP_QSTR_lorawan) },
    { MP_ROM_QSTR(MP_QSTR_version),   MP_ROM_PTR(&lorawan_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_test_hal),  MP_ROM_PTR(&lorawan_test_hal_obj) },
    { MP_ROM_QSTR(MP_QSTR_LoRaWAN),   MP_ROM_PTR(&lorawan_type) },
    // Region constants
    { MP_ROM_QSTR(MP_QSTR_EU868),     MP_ROM_INT(LORAMAC_REGION_EU868) },
    { MP_ROM_QSTR(MP_QSTR_EU433),     MP_ROM_INT(LORAMAC_REGION_EU433) },
    // Device class constants
    { MP_ROM_QSTR(MP_QSTR_CLASS_A),   MP_ROM_INT(CLASS_A) },
    { MP_ROM_QSTR(MP_QSTR_CLASS_B),   MP_ROM_INT(CLASS_B) },
    { MP_ROM_QSTR(MP_QSTR_CLASS_C),   MP_ROM_INT(CLASS_C) },
};
static MP_DEFINE_CONST_DICT(lorawan_module_globals, lorawan_module_globals_table);

const mp_obj_module_t lorawan_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lorawan_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lorawan, lorawan_module);
