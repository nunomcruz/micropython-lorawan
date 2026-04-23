// modlorawan.c — LoRaWAN Python bindings
// Phase 4, Sessions 7-10: lorawan_obj_t, __init__, join_abp, join_otaa, send, stats,
//                         recv, on_rx, on_tx_done, confirmed uplink ACK tracking,
//                         nvram_save/restore (ESP32 NVS via MIB_NVM_CTXS),
//                         datarate/adr/tx_power getters+setters.
// Phase 5, Session 11:   runtime pin config; lorawan_version (1.0.4/1.1), rx2_dr/freq,
//                         tx_power kwargs on __init__; nwk_key kwarg on join_otaa;
//                         request_class / device_class / on_class_change (Class A <-> C);
//                         antenna_gain kwarg + getter/setter (MIB_ANTENNA_GAIN).
// FreeRTOS task owns all MAC calls; Python thread communicates via queue + event group.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>

#include "py/runtime.h"
#include "py/obj.h"
#include "py/mperrno.h"

#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include "utilities.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "radio_select.h"
#include "pin_config.h"
#include "timer.h"
#include "board.h"
#include "lorawan_config.h"
#include "LoRaMac.h"
#include "region/RegionEU868.h"

// Note: we deliberately do NOT include sx1276-board.h / sx126x-board.h /
// the chip-level sx1276.h / sx126x.h here.  Those headers define several
// macros (REG_LR_SYNCWORD, LORA_MAC_PRIVATE_SYNCWORD, LORA_MAC_PUBLIC_SYNCWORD,
// RADIO_WAKEUP_TIME, REG_OCP, REG_LR_PAYLOADLENGTH) with chip-specific values
// that collide when both headers land in the same translation unit.
// The three entry points modlorawan.c actually needs — SX1276 probe,
// SX126x bring-up, and the chip-agnostic Radio table selection — are all
// exposed via hal/radio_select.h.

// ---- Constants ----

// 8 KB: LoRaMacInitialization() → SX1276Init() → RxChainCalibration() is a
// deep call chain; 4 KB was tight with the 222-byte payload on the stack.
#define LORAWAN_TASK_STACK  8192
#define LORAWAN_TASK_PRIO   6
#define LORAWAN_CMD_QSIZE   8
#define LORAWAN_RX_QSIZE    4

// Max payload: DR5 EU868 = 222 bytes
#define LW_PAYLOAD_MAX  222

// NVS namespace and key for MAC context persistence
#define LW_NVS_NAMESPACE  "lorawan"
#define LW_NVS_KEY        "nvm_ctx"

// LoRaWAN version selection (lorawan_obj_t.lorawan_version)
#define LORAWAN_V1_0_4  0
#define LORAWAN_V1_1    1

// Event group bits
#define EVT_COMPLETED    (1u << 0)
#define EVT_INIT_ERROR   (1u << 1)
#define EVT_TX_DONE      (1u << 2)
#define EVT_TX_ERROR     (1u << 3)
#define EVT_JOIN_DONE    (1u << 4)
#define EVT_NVRAM_OK     (1u << 5)
#define EVT_NVRAM_ERROR  (1u << 6)
#define EVT_CLASS_OK     (1u << 7)
#define EVT_CLASS_ERROR  (1u << 8)

// ---- Command types ----

typedef enum {
    CMD_INIT = 0,
    CMD_JOIN_ABP,
    CMD_JOIN_OTAA,
    CMD_TX,
    CMD_NVRAM_SAVE,
    CMD_NVRAM_RESTORE,
    CMD_SET_PARAMS,
    CMD_SET_CLASS,
} lorawan_cmd_t;

typedef struct {
    uint32_t dev_addr;
    uint8_t  nwk_s_key[16];
    uint8_t  app_s_key[16];
} cmd_join_abp_t;

typedef struct {
    uint8_t dev_eui[8];
    uint8_t join_eui[8];
    uint8_t app_key[16];
    uint8_t nwk_key[16]; // for 1.1: separate root key; for 1.0.x: copy of app_key
} cmd_join_otaa_t;

typedef struct {
    uint8_t  data[LW_PAYLOAD_MAX];
    uint8_t  len;
    uint8_t  port;
    uint8_t  datarate;
    bool     confirmed;
} cmd_tx_t;

// set_params.type: 0=ADR, 1=DR, 2=TX_POWER, 3=ANTENNA_GAIN
typedef struct {
    uint8_t type;
    union {
        bool   adr;
        int8_t dr;
        int8_t tx_power;
        float  antenna_gain;
    };
} cmd_set_params_t;

typedef struct {
    lorawan_cmd_t cmd;
    union {
        cmd_join_abp_t  join_abp;
        cmd_join_otaa_t join_otaa;
        cmd_tx_t        tx;
        cmd_set_params_t set_params;
        uint8_t         device_class; // CMD_SET_CLASS: DeviceClass_t value
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
    // LoRaWAN configuration (set at __init__, applied in CMD_INIT)
    uint8_t         lorawan_version; // LORAWAN_V1_0_4 or LORAWAN_V1_1
    uint8_t         rx2_dr;          // RX2 data rate index (e.g. DR_3 for TTN)
    uint32_t        rx2_freq;        // RX2 frequency in Hz
    // Python callbacks
    mp_obj_t        rx_callback;
    mp_obj_t        tx_callback;
    mp_obj_t        class_callback;
    // Last uplink stats
    uint32_t        tx_counter;
    uint32_t        tx_time_on_air;
    bool            last_tx_ack;    // confirmed uplink: true if network ACKed
    // Last downlink stats
    uint32_t        rx_counter;
    int16_t         rssi;
    int8_t          snr;
    // Cached MAC parameters (updated on init and after each TX when ADR is on)
    int8_t          channels_datarate;
    bool            adr_enabled;
    int8_t          channels_tx_power; // TX_POWER index (0=max); convert to/from dBm at API boundary
    // Antenna gain in dBi. MAC subtracts this from EIRP when computing radio TX power:
    //   radioTxPower = floor(maxEirp - txPowerIndex*2 - antennaGain)
    // EU868 region default is 2.15 dBi; we default to 0.0 so the radio emits full EIRP
    // unless the user explicitly declares a gain.
    float           antenna_gain;
} lorawan_obj_t;

// ---- Static FreeRTOS state (one LoRaWAN instance per firmware) ----

static QueueHandle_t      s_cmd_queue;
static QueueHandle_t      s_rx_queue;
static EventGroupHandle_t s_events;
static TaskHandle_t       s_task_handle;

static LoRaMacPrimitives_t s_mac_primitives;
static LoRaMacCallback_t   s_mac_callbacks;

static lorawan_obj_t *s_lora_obj;        // weak ref — valid while the object lives
static volatile bool  s_mac_initialized; // set true after LoRaMacInitialization() succeeds

// OTAA join state — written by Python thread (join_otaa), read/cleared in lorawan_task
static volatile bool s_otaa_active;          // join in progress; cleared on success or Python timeout
static volatile bool s_otaa_retry_needed;    // mlme_confirm(JOIN_FAIL) sets this; task re-sends
static volatile bool s_nvram_autosave_needed; // mlme_confirm(JOIN_OK) sets this; task saves after LoRaMacProcess()
static uint8_t       s_otaa_dev_eui[8];
static uint8_t       s_otaa_join_eui[8];
static uint8_t       s_otaa_app_key[16];
static uint8_t       s_otaa_nwk_key[16]; // 1.0.x: same as app_key; 1.1: separate root key

// ---- TX power dBm ↔ MAC index conversion ----
//
// EU868 EIRP table: index 0 = 16 dBm, each step down = -2 dBm, max index 7 = 2 dBm.
// Conversion rounds down (never exceeds requested power, important for regulatory compliance).
// Other regions may have different tables; this helper is EU868-accurate for now.

static int tx_power_to_dbm(int8_t index) {
    int8_t i = (index < 0) ? 0 : (index > 7) ? 7 : index;
    return 16 - 2 * (int)i;
}

static int8_t dbm_to_tx_power(int dbm) {
    // eirp(i) = 16 - 2i  =>  i = ceil((16 - dbm) / 2.0)
    // Integer: (16 - dbm + 1) / 2, then clamp to [0, 7]
    if (dbm >= 16) return 0;
    if (dbm <= 2)  return 7;
    int8_t i = (int8_t)((16 - dbm + 1) / 2);
    return (i > 7) ? 7 : i;
}

// ---- Scheduler trampolines (run in MicroPython VM context) ----
//
// mp_sched_schedule schedules these from the LoRaWAN task (CPU1, no GIL).
// They execute on the Python thread (CPU0, holds GIL), so it is safe to
// allocate MicroPython objects and call Python functions here.

// Called when a downlink packet has been pushed to s_rx_queue.
// Pops one packet and invokes the stored on_rx callback.
static mp_obj_t lorawan_rx_trampoline(mp_obj_t arg) {
    (void)arg;
    if (!s_lora_obj || s_lora_obj->rx_callback == mp_const_none || !s_rx_queue) {
        return mp_const_none;
    }
    lorawan_rx_pkt_t pkt;
    if (xQueueReceive(s_rx_queue, &pkt, 0) != pdTRUE) {
        return mp_const_none;
    }
    mp_obj_t items[4];
    items[0] = mp_obj_new_bytes(pkt.data, pkt.len);
    items[1] = mp_obj_new_int(pkt.port);
    items[2] = mp_obj_new_int(pkt.rssi);
    items[3] = mp_obj_new_int(pkt.snr);
    mp_call_function_1(s_lora_obj->rx_callback, mp_obj_new_tuple(4, items));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_rx_trampoline_obj, lorawan_rx_trampoline);

// Called when an uplink TX cycle completes.
// arg is mp_const_true (success / ACK received) or mp_const_false (failure).
static mp_obj_t lorawan_tx_trampoline(mp_obj_t arg) {
    if (!s_lora_obj || s_lora_obj->tx_callback == mp_const_none) {
        return mp_const_none;
    }
    mp_call_function_1(s_lora_obj->tx_callback, arg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_tx_trampoline_obj, lorawan_tx_trampoline);

// Called when a device-class transition is confirmed.
// arg is the new class as a small int (CLASS_A, CLASS_B, CLASS_C).
static mp_obj_t lorawan_class_trampoline(mp_obj_t arg) {
    if (!s_lora_obj || s_lora_obj->class_callback == mp_const_none) {
        return mp_const_none;
    }
    mp_call_function_1(s_lora_obj->class_callback, arg);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_class_trampoline_obj, lorawan_class_trampoline);

// ---- LoRaMAC MAC-layer callbacks (run in the LoRaWAN task context) ----

static void mcps_confirm(McpsConfirm_t *c) {
    if (!c) return;

    esp_rom_printf("lorawan: mcps_confirm status=%d uplink=%lu toa=%lu\n",
                   (int)c->Status, (unsigned long)c->UpLinkCounter,
                   (unsigned long)c->TxTimeOnAir);

    bool tx_ok = (c->Status == LORAMAC_EVENT_INFO_STATUS_OK);

    if (tx_ok) {
        if (s_lora_obj) {
            s_lora_obj->tx_counter     = c->UpLinkCounter;
            s_lora_obj->tx_time_on_air = c->TxTimeOnAir;
            // AckReceived is only meaningful for MCPS_CONFIRMED frames.
            s_lora_obj->last_tx_ack    = c->AckReceived;

            // When ADR is on the MAC may have adjusted the DR after this TX.
            // Read it back so the Python datarate() getter stays accurate.
            if (s_lora_obj->adr_enabled) {
                MibRequestConfirm_t dr_mib;
                dr_mib.Type = MIB_CHANNELS_DATARATE;
                if (LoRaMacMibGetRequestConfirm(&dr_mib) == LORAMAC_STATUS_OK) {
                    s_lora_obj->channels_datarate = dr_mib.Param.ChannelsDatarate;
                }
            }
        }
        xEventGroupSetBits(s_events, EVT_TX_DONE);
    } else {
        if (s_lora_obj) s_lora_obj->last_tx_ack = false;
        xEventGroupSetBits(s_events, EVT_TX_ERROR);
    }

    // Schedule Python on_tx_done callback if registered.
    // For confirmed uplinks success = ACK received; for unconfirmed = frame sent.
    if (s_lora_obj && s_lora_obj->tx_callback != mp_const_none) {
        bool success = (c->McpsRequest == MCPS_CONFIRMED)
                       ? (tx_ok && c->AckReceived)
                       : tx_ok;
        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_tx_trampoline_obj),
                          success ? mp_const_true : mp_const_false);
    }
}

static void mcps_indication(McpsIndication_t *ind) {
    if (!ind || ind->Status != LORAMAC_EVENT_INFO_STATUS_OK) return;

    if (s_lora_obj) {
        s_lora_obj->rssi       = ind->Rssi;
        s_lora_obj->snr        = ind->Snr;
        s_lora_obj->rx_counter = ind->DownLinkCounter;
    }

    if (!ind->RxData || ind->BufferSize == 0) return;
    if (ind->Port == 0 || ind->Port >= 224) return;

    lorawan_rx_pkt_t pkt;
    pkt.len  = (ind->BufferSize <= LW_PAYLOAD_MAX) ? (uint8_t)ind->BufferSize : LW_PAYLOAD_MAX;
    pkt.port = ind->Port;
    pkt.rssi = ind->Rssi;
    pkt.snr  = ind->Snr;
    memcpy(pkt.data, ind->Buffer, pkt.len);
    xQueueSend(s_rx_queue, &pkt, 0);

    // Schedule Python on_rx callback if registered.
    // The trampoline pops from s_rx_queue and calls the callback in Python context.
    if (s_lora_obj && s_lora_obj->rx_callback != mp_const_none) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_rx_trampoline_obj), mp_const_none);
    }
}

static void mlme_confirm(MlmeConfirm_t *c) {
    if (!c) return;
    if (c->MlmeRequest != MLME_JOIN) return;

    if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
        esp_rom_printf("lorawan: OTAA join accepted\n");
        if (s_lora_obj) s_lora_obj->joined = true;
        s_otaa_active = false;
        s_nvram_autosave_needed = true;
        xEventGroupSetBits(s_events, EVT_JOIN_DONE);
    } else {
        esp_rom_printf("lorawan: OTAA join attempt failed (status=%d), retrying\n",
                       (int)c->Status);
        // Set flag; lorawan_task re-sends MLME_JOIN on next iteration so we
        // stay out of a LoRaMAC callback when calling LoRaMacMlmeRequest.
        if (s_otaa_active) {
            s_otaa_retry_needed = true;
        }
    }
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

// ---- NVS helpers (called from lorawan_task only) ----

// Save full NVM context to ESP32 NVS. Returns true on success.
// Called from CMD_NVRAM_SAVE (Python-triggered) and after a successful OTAA join.
// CRCs are recomputed unconditionally: LoRaMacHandleNvm() only runs from
// LoRaMacProcess() when MacState==IDLE, so there is a window where they are stale.
static bool nvs_save_nvm_ctx(void) {
    MibRequestConfirm_t mib;
    mib.Type = MIB_NVM_CTXS;
    LoRaMacStatus_t st = LoRaMacMibGetRequestConfirm(&mib);
    if (st != LORAMAC_STATUS_OK) {
        esp_rom_printf("lorawan: nvram_save: MIB_NVM_CTXS get failed: %d\n", (int)st);
        return false;
    }
    LoRaMacNvmData_t *ctx = mib.Param.Contexts;

#define NVM_UPDATE_CRC(grp) \
    ctx->grp.Crc32 = Crc32((uint8_t *)&ctx->grp, \
                            (uint16_t)(sizeof(ctx->grp) - sizeof(ctx->grp.Crc32)))
    NVM_UPDATE_CRC(Crypto);
    NVM_UPDATE_CRC(MacGroup1);
    NVM_UPDATE_CRC(MacGroup2);
    NVM_UPDATE_CRC(SecureElement);
    NVM_UPDATE_CRC(RegionGroup1);
    NVM_UPDATE_CRC(RegionGroup2);
    NVM_UPDATE_CRC(ClassB);
#undef NVM_UPDATE_CRC

    esp_rom_printf("lorawan: nvram_save: FCntUp=%lu DevAddr=0x%08lx\n",
                   (unsigned long)ctx->Crypto.FCntList.FCntUp,
                   (unsigned long)ctx->MacGroup2.DevAddr);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(LW_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        esp_rom_printf("lorawan: nvram_save: nvs_open failed: %d\n", (int)err);
        return false;
    }
    err = nvs_set_blob(nvs, LW_NVS_KEY, ctx, sizeof(LoRaMacNvmData_t));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        esp_rom_printf("lorawan: nvram_save: NVS write failed: %d\n", (int)err);
        return false;
    }
    esp_rom_printf("lorawan: nvram_save: saved %u bytes\n", (unsigned)sizeof(LoRaMacNvmData_t));
    return true;
}

// ---- LoRaWAN task ----

static void lorawan_task(void *arg) {
    (void)arg;

    MibRequestConfirm_t mib;
    McpsReq_t           mcps;
    lorawan_cmd_data_t  cmd;

    for (;;) {
        // Sleep until MacProcessNotify wakes us or a command arrives.
        // pdMS_TO_TICKS(2) = 0 on 100 Hz → non-blocking poll causing extreme
        // kernel-lock contention that starves CPU0's xQueueSend.
        // 10 ms = 1 tick on 100 Hz: task actually sleeps and yields CPU1.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));

        // Guard: LoRaMacProcess() must not be called before the MAC stack is
        // initialised.  The CMD_INIT handler sets s_mac_initialized after
        // LoRaMacInitialization() + LoRaMacStart() succeed.
        if (s_mac_initialized) {
            // SX126x: DIO1 ISR only sets IrqFired; RadioIrqProcess() reads the
            // IRQ status register and calls RadioEvents->TxDone/RxDone which
            // populate LoRaMacRadioEvents consumed by LoRaMacProcess().
            // Must run before LoRaMacProcess() or TX_DONE is never seen by MAC,
            // TX timeout fires, and McpsConfirm gets TX_TIMEOUT (→ "send failed").
            // SX1276: IrqProcess is NULL — this is a no-op.
            lorawan_radio_irq_process();
            LoRaMacProcess();
        }

        // Persist NVM immediately after a successful OTAA join so that DevNonce
        // is never lost to a reset between join and the first Python nvram_save().
        if (s_nvram_autosave_needed) {
            s_nvram_autosave_needed = false;
            nvs_save_nvm_ctx();
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

                // RX2 window: use per-instance configuration.
                // Region-specific defaults set in lorawan_make_new; freq=0 means
                // "skip override, let the MAC use the region's PHY_DEF_RX2".
                uint32_t rx2_freq = s_lora_obj ? s_lora_obj->rx2_freq : 869525000;
                uint8_t  rx2_dr   = s_lora_obj ? s_lora_obj->rx2_dr   : DR_3;
                if (rx2_freq != 0) {
                    mib.Type = MIB_RX2_CHANNEL;
                    mib.Param.Rx2Channel.Frequency = rx2_freq;
                    mib.Param.Rx2Channel.Datarate  = rx2_dr;
                    LoRaMacMibSetRequestConfirm(&mib);

                    mib.Type = MIB_RX2_DEFAULT_CHANNEL;
                    mib.Param.Rx2DefaultChannel.Frequency = rx2_freq;
                    mib.Param.Rx2DefaultChannel.Datarate  = rx2_dr;
                    LoRaMacMibSetRequestConfirm(&mib);
                    esp_rom_printf("lorawan: RX2 freq=%lu DR=%u\n",
                                   (unsigned long)rx2_freq, (unsigned)rx2_dr);
                } else {
                    esp_rom_printf("lorawan: RX2 using region default\n");
                }

                // Class C continuous listen uses a separate RxCChannel (not Rx2Channel).
                // In LoRaMAC-node v4.7.0 the RxCChannel defaults to PHY_DEF_RX2_DR
                // / PHY_DEF_RX2_FREQUENCY, which for EU868 is DR_0 (SF12) / 869.525 MHz —
                // matching TTN's Class C downlink scheduling. Do NOT mirror rx2_dr into
                // RxCChannel: TTN sends Class A RX2 at DR_3 but Class C at DR_0, so they
                // must not share the same DR. If a non-TTN network needs a different
                // Class C DR, add an rxc_dr kwarg; for now rely on the LoRaMAC defaults.
                //
                // After an OTAA JoinAccept, LoRaMac.c:1034 updates RxCChannel.Datarate
                // from DLSettings so the negotiated RX2 DR applies to both windows.

                // Apply user-specified initial TX power (if non-default).
                // channels_tx_power is a MAC index; 0 = max (16 dBm EU868).
                if (s_lora_obj && s_lora_obj->channels_tx_power != 0) {
                    mib.Type = MIB_CHANNELS_TX_POWER;
                    mib.Param.ChannelsTxPower = s_lora_obj->channels_tx_power;
                    LoRaMacMibSetRequestConfirm(&mib);
                }

                // Apply user-specified antenna gain (default 0.0 dBi).
                // EU868 region default is 2.15 dBi, which causes the MAC to
                // under-drive the radio by 2.15 dB relative to the requested EIRP.
                // Set both the current and the "default" MIB so that any internal
                // ResetMacParameters (e.g. on deactivation) keeps our value.
                if (s_lora_obj) {
                    mib.Type = MIB_ANTENNA_GAIN;
                    mib.Param.AntennaGain = s_lora_obj->antenna_gain;
                    LoRaMacMibSetRequestConfirm(&mib);

                    mib.Type = MIB_DEFAULT_ANTENNA_GAIN;
                    mib.Param.DefaultAntennaGain = s_lora_obj->antenna_gain;
                    LoRaMacMibSetRequestConfirm(&mib);
                }

                // Cache initial MAC parameter values into the Python object.
                if (s_lora_obj) {
                    mib.Type = MIB_ADR;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->adr_enabled = mib.Param.AdrEnable;

                    mib.Type = MIB_CHANNELS_DATARATE;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->channels_datarate = mib.Param.ChannelsDatarate;

                    mib.Type = MIB_CHANNELS_TX_POWER;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->channels_tx_power = mib.Param.ChannelsTxPower;
                }

                s_mac_initialized = true;
                if (s_lora_obj) s_lora_obj->initialized = true;
                esp_rom_printf("lorawan: init done\n");
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_JOIN_ABP: {
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

                // Set ABP LoRaWAN version for MIC computation.
                // 1.0.4 uses single-key MIC; 1.1 uses two-key MIC.
                // LoRaMAC-node defaults to 1.1.1 — must be set explicitly.
                bool is_v11 = s_lora_obj && (s_lora_obj->lorawan_version == LORAWAN_V1_1);
                mib.Type = MIB_ABP_LORAWAN_VERSION;
                mib.Param.AbpLrWanVersion.Fields.Major    = 1;
                mib.Param.AbpLrWanVersion.Fields.Minor    = is_v11 ? 1 : 0;
                mib.Param.AbpLrWanVersion.Fields.Patch    = is_v11 ? 0 : 4;
                mib.Param.AbpLrWanVersion.Fields.Revision = 0;
                LoRaMacMibSetRequestConfirm(&mib);

                if (s_lora_obj) s_lora_obj->joined = true;
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_JOIN_OTAA: {
                // Store credentials for retry loop (mlme_confirm re-reads them).
                memcpy(s_otaa_dev_eui,  cmd.join_otaa.dev_eui,  8);
                memcpy(s_otaa_join_eui, cmd.join_otaa.join_eui, 8);
                memcpy(s_otaa_app_key,  cmd.join_otaa.app_key,  16);
                memcpy(s_otaa_nwk_key,  cmd.join_otaa.nwk_key,  16);

                mib.Type = MIB_NETWORK_ACTIVATION;
                mib.Param.NetworkActivation = ACTIVATION_TYPE_OTAA;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_DEV_EUI;
                mib.Param.DevEui = s_otaa_dev_eui;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_JOIN_EUI;
                mib.Param.JoinEui = s_otaa_join_eui;
                LoRaMacMibSetRequestConfirm(&mib);

                mib.Type = MIB_APP_KEY;
                mib.Param.AppKey = s_otaa_app_key;
                LoRaMacMibSetRequestConfirm(&mib);

                // LoRaMAC-node always uses NwkKey for the Join Request MIC.
                // 1.0.x: NwkKey = AppKey (single root key). 1.1: separate NwkKey.
                mib.Type = MIB_NWK_KEY;
                mib.Param.NwkKey = s_otaa_nwk_key;
                LoRaMacMibSetRequestConfirm(&mib);

                s_otaa_active       = true;
                s_otaa_retry_needed = false;

                MlmeReq_t mlme;
                mlme.Type                       = MLME_JOIN;
                mlme.Req.Join.NetworkActivation = ACTIVATION_TYPE_OTAA;
                mlme.Req.Join.Datarate          = DR_0;
                LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: MLME_JOIN request error %d\n", (int)st);
                    if (st == LORAMAC_STATUS_DUTYCYCLE_RESTRICTED) {
                        // Will retry when duty-cycle clears; leave s_otaa_active set.
                        s_otaa_retry_needed = true;
                    } else {
                        s_otaa_active = false;
                    }
                } else {
                    esp_rom_printf("lorawan: OTAA join request sent\n");
                }
                // Signal Python that the request was accepted (or queued for retry).
                // Python then waits separately on EVT_JOIN_DONE with its own timeout.
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
                    mcps.Req.Confirmed.Datarate    = cmd.tx.datarate;
                } else {
                    mcps.Type = MCPS_UNCONFIRMED;
                    mcps.Req.Unconfirmed.fPort       = cmd.tx.port;
                    mcps.Req.Unconfirmed.fBuffer     = cmd.tx.data;
                    mcps.Req.Unconfirmed.fBufferSize = cmd.tx.len;
                    mcps.Req.Unconfirmed.Datarate    = cmd.tx.datarate;
                }

                LoRaMacStatus_t st = LoRaMacMcpsRequest(&mcps);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: McpsRequest failed: %d\n", (int)st);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                }
                // EVT_TX_DONE / EVT_TX_ERROR set asynchronously by mcps_confirm
                break;
            }

            case CMD_NVRAM_SAVE: {
                bool ok = nvs_save_nvm_ctx();
                xEventGroupSetBits(s_events, ok ? EVT_NVRAM_OK : EVT_NVRAM_ERROR);
                break;
            }

            case CMD_NVRAM_RESTORE: {
                // Static buffer: large struct; avoid stack allocation.
                static LoRaMacNvmData_t s_restored_ctx;
                nvs_handle_t nvs;
                esp_err_t err = nvs_open(LW_NVS_NAMESPACE, NVS_READONLY, &nvs);
                if (err != ESP_OK) {
                    esp_rom_printf("lorawan: nvram_restore: nvs_open failed: %d\n", (int)err);
                    xEventGroupSetBits(s_events, EVT_NVRAM_ERROR);
                    break;
                }
                size_t size = sizeof(LoRaMacNvmData_t);
                err = nvs_get_blob(nvs, LW_NVS_KEY, &s_restored_ctx, &size);
                nvs_close(nvs);
                if (err != ESP_OK) {
                    esp_rom_printf("lorawan: nvram_restore: NVS read failed: %d\n", (int)err);
                    xEventGroupSetBits(s_events, EVT_NVRAM_ERROR);
                    break;
                }

                // RestoreNvmData() requires MacState == LORAMAC_STOPPED (not IDLE).
                // Stop the MAC, restore, restart. This is safe because the Python
                // thread is blocked in xEventGroupWaitBits so no TX can be queued.
                LoRaMacStop();

                mib.Type = MIB_NVM_CTXS;
                mib.Param.Contexts = &s_restored_ctx;
                LoRaMacStatus_t st = LoRaMacMibSetRequestConfirm(&mib);

                // Always restart the MAC — we must not leave it stopped.
                LoRaMacStart();

                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: nvram_restore: MIB_NVM_CTXS set failed: %d\n", (int)st);
                    xEventGroupSetBits(s_events, EVT_NVRAM_ERROR);
                    break;
                }

                // Do NOT re-apply RX2 or channel config here. The Join Accept
                // DLSettings field sets Rx2DataRate during OTAA; that value is
                // stored in MacGroup2 and restored by RestoreNvmData. Overriding
                // with hardcoded values would undo the join-negotiated parameters.

                // Sync Python-side cached state from the restored MAC params.
                if (s_lora_obj) {
                    s_lora_obj->joined =
                        (s_restored_ctx.MacGroup2.NetworkActivation != ACTIVATION_TYPE_NONE);

                    mib.Type = MIB_CHANNELS_DATARATE;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->channels_datarate = mib.Param.ChannelsDatarate;

                    mib.Type = MIB_ADR;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->adr_enabled = mib.Param.AdrEnable;

                    mib.Type = MIB_CHANNELS_TX_POWER;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->channels_tx_power = mib.Param.ChannelsTxPower;

                    // Device class is stored in MacGroup2.DeviceClass; the MIB
                    // getter returns the live value which mirrors NVM.
                    mib.Type = MIB_DEVICE_CLASS;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->device_class = mib.Param.Class;

                    // Antenna gain was saved in MacGroup2.MacParams — sync the
                    // Python-side cache so antenna_gain() returns the restored value.
                    mib.Type = MIB_ANTENNA_GAIN;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->antenna_gain = mib.Param.AntennaGain;
                }
                // Read FCntUp back from the MAC to confirm the Crypto group was
                // restored (CRC matched).  If FCntUp==0 here, the CRC was wrong.
                mib.Type = MIB_NVM_CTXS;
                if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: nvram_restore: FCntUp=%lu DevAddr=0x%08lx joined=%d\n",
                                   (unsigned long)mib.Param.Contexts->Crypto.FCntList.FCntUp,
                                   (unsigned long)mib.Param.Contexts->MacGroup2.DevAddr,
                                   s_lora_obj ? (int)s_lora_obj->joined : 0);
                } else {
                    esp_rom_printf("lorawan: nvram_restore: done, joined=%d\n",
                                   s_lora_obj ? (int)s_lora_obj->joined : 0);
                }
                xEventGroupSetBits(s_events, EVT_NVRAM_OK);
                break;
            }

            case CMD_SET_PARAMS: {
                switch (cmd.set_params.type) {
                case 0: // ADR
                    mib.Type = MIB_ADR;
                    mib.Param.AdrEnable = cmd.set_params.adr;
                    LoRaMacMibSetRequestConfirm(&mib);
                    if (s_lora_obj) s_lora_obj->adr_enabled = cmd.set_params.adr;
                    break;
                case 1: // Datarate
                    mib.Type = MIB_CHANNELS_DATARATE;
                    mib.Param.ChannelsDatarate = cmd.set_params.dr;
                    LoRaMacMibSetRequestConfirm(&mib);
                    if (s_lora_obj) s_lora_obj->channels_datarate = cmd.set_params.dr;
                    break;
                case 2: // TX power
                    mib.Type = MIB_CHANNELS_TX_POWER;
                    mib.Param.ChannelsTxPower = cmd.set_params.tx_power;
                    LoRaMacMibSetRequestConfirm(&mib);
                    if (s_lora_obj) s_lora_obj->channels_tx_power = cmd.set_params.tx_power;
                    break;
                case 3: // Antenna gain (current + default, so a subsequent
                        // ResetMacParameters doesn't restore the region default)
                    mib.Type = MIB_ANTENNA_GAIN;
                    mib.Param.AntennaGain = cmd.set_params.antenna_gain;
                    LoRaMacMibSetRequestConfirm(&mib);
                    mib.Type = MIB_DEFAULT_ANTENNA_GAIN;
                    mib.Param.DefaultAntennaGain = cmd.set_params.antenna_gain;
                    LoRaMacMibSetRequestConfirm(&mib);
                    if (s_lora_obj) s_lora_obj->antenna_gain = cmd.set_params.antenna_gain;
                    break;
                default:
                    break;
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_SET_CLASS: {
                // LoRaMAC-node v4.7.0 does not expose LoRaMacRequestClass / MLME_CLASS_C_SWITCH.
                // Class A <-> Class C switches are done via MIB_DEVICE_CLASS and are
                // instantaneous (see LmHandler.c: LmHandlerRequestClass).
                // Class B requires DeviceTime + beacon acquisition (Session 13).
                DeviceClass_t new_class = (DeviceClass_t)cmd.device_class;

                if (new_class == CLASS_B) {
                    esp_rom_printf("lorawan: CLASS_B switch not implemented yet\n");
                    xEventGroupSetBits(s_events, EVT_CLASS_ERROR);
                    break;
                }

                // MIB_DEVICE_CLASS set fails if current class is B and the target
                // is anything other than A. We allow only A <-> C here.
                mib.Type = MIB_DEVICE_CLASS;
                LoRaMacMibGetRequestConfirm(&mib);
                DeviceClass_t cur_class = mib.Param.Class;

                if (cur_class == new_class) {
                    // No-op: already in the requested class.
                    xEventGroupSetBits(s_events, EVT_CLASS_OK);
                    break;
                }

                mib.Type = MIB_DEVICE_CLASS;
                mib.Param.Class = new_class;
                LoRaMacStatus_t st = LoRaMacMibSetRequestConfirm(&mib);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: MIB_DEVICE_CLASS set failed: %d\n", (int)st);
                    xEventGroupSetBits(s_events, EVT_CLASS_ERROR);
                    break;
                }

                if (s_lora_obj) {
                    s_lora_obj->device_class = new_class;
                    if (s_lora_obj->class_callback != mp_const_none) {
                        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_class_trampoline_obj),
                                          MP_OBJ_NEW_SMALL_INT((mp_int_t)new_class));
                    }
                }
                esp_rom_printf("lorawan: device class -> %c\n",
                               new_class == CLASS_A ? 'A' : (new_class == CLASS_B ? 'B' : 'C'));

                // Diagnostic: log the actual RxC params the MAC is using for the
                // continuous listen, so a SF mismatch with the network can be spotted.
                if (new_class == CLASS_C) {
                    mib.Type = MIB_RXC_CHANNEL;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
                        esp_rom_printf("lorawan: RxC freq=%lu DR=%u\n",
                                       (unsigned long)mib.Param.RxCChannel.Frequency,
                                       (unsigned)mib.Param.RxCChannel.Datarate);
                    }
                }
                xEventGroupSetBits(s_events, EVT_CLASS_OK);
                break;
            }

            default:
                break;
            }
        }

        // OTAA retry: mlme_confirm set s_otaa_retry_needed after a failed join
        // attempt.  Re-issue MLME_JOIN here (task context, not callback context).
        // On DUTYCYCLE_RESTRICTED, MacProcessNotify will wake us again when the
        // duty-cycle timer clears; leave s_otaa_retry_needed set so we try again.
        if (s_otaa_retry_needed && s_otaa_active) {
            MlmeReq_t mlme;
            mlme.Type                        = MLME_JOIN;
            mlme.Req.Join.NetworkActivation  = ACTIVATION_TYPE_OTAA;
            mlme.Req.Join.Datarate           = DR_0;
            LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
            if (st == LORAMAC_STATUS_OK) {
                s_otaa_retry_needed = false;
            } else if (st != LORAMAC_STATUS_DUTYCYCLE_RESTRICTED) {
                // Permanent MAC error — stop retrying so Python can timeout.
                esp_rom_printf("lorawan: MLME_JOIN retry error %d\n", (int)st);
                s_otaa_retry_needed = false;
                s_otaa_active       = false;
            }
            // DUTYCYCLE_RESTRICTED: leave s_otaa_retry_needed = true, try again next wake.
        }
    }
}

// ---- Internal helpers ----

static void send_cmd_wait(lorawan_cmd_data_t *cmd, EventBits_t wait_bits) {
    xEventGroupClearBits(s_events, wait_bits);
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    xTaskNotifyGive(s_task_handle);
    xEventGroupWaitBits(s_events, wait_bits, pdTRUE, pdFALSE, portMAX_DELAY);
}

// Like send_cmd_wait but returns the EventBits_t so callers can distinguish
// success from error bits (e.g. EVT_NVRAM_OK vs EVT_NVRAM_ERROR).
static EventBits_t send_cmd_wait_result(lorawan_cmd_data_t *cmd, EventBits_t wait_bits) {
    xEventGroupClearBits(s_events, wait_bits);
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    xTaskNotifyGive(s_task_handle);
    return xEventGroupWaitBits(s_events, wait_bits, pdTRUE, pdFALSE, portMAX_DELAY);
}

// ---- Python type: lorawan.LoRaWAN ----

static mp_obj_t lorawan_make_new(const mp_obj_type_t *type,
                                  size_t n_args, size_t n_kw,
                                  const mp_obj_t *all_args) {
    enum {
        ARG_region, ARG_radio, ARG_device_class,
        ARG_spi_id, ARG_mosi, ARG_miso, ARG_sclk,
        ARG_cs, ARG_reset, ARG_irq, ARG_busy,
        ARG_lorawan_version, ARG_rx2_dr, ARG_rx2_freq, ARG_tx_power,
        ARG_antenna_gain,
    };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_region,           MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_radio,            MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_device_class,     MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = CLASS_A} },
        // Pin overrides — -1 means "use T-Beam default from lorawan_config.h"
        { MP_QSTR_spi_id,           MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_mosi,             MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_miso,             MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_sclk,             MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_cs,               MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_reset,            MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_irq,              MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_busy,             MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        // LoRaWAN protocol version — affects MIC computation and key derivation
        { MP_QSTR_lorawan_version,  MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = LORAWAN_V1_0_4} },
        // RX2 window — region-aware defaults (see rx2 resolution below).
        // EU868: 869.525 MHz / DR_3 (TTN convention; standard LoRaWAN EU868 is DR_0).
        // EU433: 434.665 MHz / DR_0 (EU433_RX_WND_2).
        // Sentinel -1 means "use region default"; pass explicit values to override.
        { MP_QSTR_rx2_dr,           MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_rx2_freq,         MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = -1} },
        // TX power in dBm EIRP. None = use region maximum (16 dBm for EU868).
        // Internally converted to MAC TX_POWER index; getter also returns dBm.
        { MP_QSTR_tx_power,         MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        // Antenna gain in dBi. Default 0.0 so the radio emits the full EIRP.
        // The EU868 region default (2.15 dBi) would otherwise cause the MAC to
        // subtract 2.15 from the requested EIRP, silently under-driving the radio.
        { MP_QSTR_antenna_gain,     MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args,
                               MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (s_task_handle != NULL) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("LoRaWAN already initialized; reset first"));
    }

    lorawan_obj_t *self = mp_obj_malloc(lorawan_obj_t, type);
    self->initialized      = false;
    self->joined           = false;
    self->is_sx1276        = true;
    self->region           = (LoRaMacRegion_t)args[ARG_region].u_int;
    self->device_class     = (DeviceClass_t)args[ARG_device_class].u_int;
    self->lorawan_version  = (uint8_t)args[ARG_lorawan_version].u_int;

    // RX2 defaults depend on region; sentinel -1 means "use region default".
    int      rx2_dr_arg   = args[ARG_rx2_dr].u_int;
    int      rx2_freq_arg = args[ARG_rx2_freq].u_int;
    uint8_t  rx2_dr_def   = DR_0;
    uint32_t rx2_freq_def = 0;
    switch (self->region) {
        case LORAMAC_REGION_EU868:
            // TTN EU868 uses DR_3 (SF9) on 869.525 MHz; standard LoRaWAN is DR_0.
            rx2_dr_def   = DR_3;
            rx2_freq_def = 869525000;
            break;
        case LORAMAC_REGION_EU433:
            rx2_dr_def   = DR_0;        // EU433_RX_WND_2_DR
            rx2_freq_def = 434665000;   // EU433_RX_WND_2_FREQ
            break;
        default:
            // Other regions: fall through with DR_0 / freq=0. freq=0 is invalid,
            // so the CMD_INIT path below will skip MIB_RX2_CHANNEL and let the
            // MAC use its region-default RX2 parameters.
            break;
    }
    self->rx2_dr   = (rx2_dr_arg   < 0) ? rx2_dr_def   : (uint8_t)rx2_dr_arg;
    self->rx2_freq = (rx2_freq_arg < 0) ? rx2_freq_def : (uint32_t)rx2_freq_arg;
    self->rx_callback      = mp_const_none;
    self->tx_callback      = mp_const_none;
    self->class_callback   = mp_const_none;
    self->tx_counter       = 0;
    self->tx_time_on_air   = 0;
    self->last_tx_ack      = false;
    self->rx_counter       = 0;
    self->rssi             = 0;
    self->snr              = 0;
    self->channels_datarate = DR_0;
    self->adr_enabled       = false;
    // tx_power kwarg: None = use region max (index 0 = 16 dBm EU868); int = dBm.
    // If dBm exceeds the region table max, activate hardware override (user's responsibility).
    if (args[ARG_tx_power].u_obj != mp_const_none) {
        int dbm = (int)mp_obj_get_int(args[ARG_tx_power].u_obj);
        int8_t idx      = dbm_to_tx_power(dbm);
        int    mac_max  = tx_power_to_dbm(0); // region table max (16 dBm EU868)
        if (dbm > mac_max) {
            g_tx_power_hw_override = (int8_t)dbm; // bypass MAC validation
            self->channels_tx_power = 0;           // MAC sees region max
        } else {
            self->channels_tx_power = idx;
        }
    } else {
        self->channels_tx_power = 0; // TX_POWER_0 = region max
    }

    // antenna_gain kwarg: None → 0.0 dBi (full EIRP at the radio).
    self->antenna_gain = (args[ARG_antenna_gain].u_obj != mp_const_none)
                         ? mp_obj_get_float(args[ARG_antenna_gain].u_obj)
                         : 0.0f;

    // Apply pin overrides to g_lorawan_pins before any IoInit.
    // -1 means "keep T-Beam default" (set in pin_config.c initialiser).
    // irq routes to the appropriate field for both radio types; only one
    // IoInit call will actually consume it.
    #define _APPLY(field, idx) \
        if (args[idx].u_int != -1) g_lorawan_pins.field = (int)args[idx].u_int
    if (args[ARG_spi_id].u_int != -1) {
        g_lorawan_pins.spi_host = (int)args[ARG_spi_id].u_int;
    }
    _APPLY(mosi,  ARG_mosi);
    _APPLY(miso,  ARG_miso);
    _APPLY(sclk,  ARG_sclk);
    _APPLY(nss,   ARG_cs);
    _APPLY(reset, ARG_reset);
    _APPLY(busy,  ARG_busy);
    if (args[ARG_irq].u_int != -1) {
        int irq_pin = (int)args[ARG_irq].u_int;
        g_lorawan_pins.dio0      = irq_pin;  // SX1276 path
        g_lorawan_pins.dio1_1262 = irq_pin;  // SX1262 path
    }
    #undef _APPLY

    // Parse radio kwarg: "sx1276" | "sx1262" | None (auto-detect).
    // True/False kept for backwards compatibility.
    bool radio_forced = false;
    bool is_sx1276_forced = false;
    if (args[ARG_radio].u_obj != mp_const_none) {
        if (mp_obj_is_str(args[ARG_radio].u_obj)) {
            const char *rs = mp_obj_str_get_str(args[ARG_radio].u_obj);
            if (strcmp(rs, "sx1276") == 0) {
                radio_forced = true; is_sx1276_forced = true;
            } else if (strcmp(rs, "sx1262") == 0) {
                radio_forced = true; is_sx1276_forced = false;
            } else {
                mp_raise_ValueError(
                    MP_ERROR_TEXT("radio: expected 'sx1276', 'sx1262', or None"));
            }
        } else {
            radio_forced = true;
            is_sx1276_forced = mp_obj_is_true(args[ARG_radio].u_obj);
        }
    }

    // Radio detection: bring up SX1276 HAL (GPIO + SPI), reset, read reg 0x42.
    // SX1276 version register returns 0x12; anything else → SX1262.
    uint8_t reg42 = lorawan_radio_probe_reg42();
    bool is_sx1276 = radio_forced ? is_sx1276_forced : (reg42 == 0x12);

    if (!is_sx1276) {
        lorawan_radio_init_sx126x();
    }

    self->is_sx1276 = is_sx1276;
    lorawan_radio_select(is_sx1276);

    mp_printf(&mp_plat_print, "lorawan: radio=%s (reg42=0x%02X)\n",
              is_sx1276 ? "SX1276" : "SX1262", reg42);

    s_lora_obj  = self;
    s_cmd_queue = xQueueCreate(LORAWAN_CMD_QSIZE, sizeof(lorawan_cmd_data_t));
    s_rx_queue  = xQueueCreate(LORAWAN_RX_QSIZE,  sizeof(lorawan_rx_pkt_t));
    s_events    = xEventGroupCreate();

    // ESP-IDF v5+ uses BYTES for usStackDepth (differs from vanilla FreeRTOS).
    // Do NOT divide by sizeof(StackType_t) here.
    BaseType_t task_ok = xTaskCreatePinnedToCore(lorawan_task, "lorawan",
                            LORAWAN_TASK_STACK,
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

static mp_obj_t lorawan_join_otaa(size_t n_args, const mp_obj_t *pos_args,
                                   mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);

    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }

    enum { ARG_dev_eui, ARG_join_eui, ARG_app_key, ARG_timeout, ARG_nwk_key };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_dev_eui,  MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_join_eui, MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_app_key,  MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_timeout,  MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 30} },
        // nwk_key: LoRaWAN 1.1 root network key. Omit for 1.0.x (NwkKey = AppKey).
        { MP_QSTR_nwk_key,  MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_buffer_info_t dev_eui_buf, join_eui_buf, app_key_buf;
    mp_get_buffer_raise(args[ARG_dev_eui].u_obj,  &dev_eui_buf,  MP_BUFFER_READ);
    mp_get_buffer_raise(args[ARG_join_eui].u_obj, &join_eui_buf, MP_BUFFER_READ);
    mp_get_buffer_raise(args[ARG_app_key].u_obj,  &app_key_buf,  MP_BUFFER_READ);

    if (dev_eui_buf.len != 8 || join_eui_buf.len != 8) {
        mp_raise_ValueError(MP_ERROR_TEXT("dev_eui and join_eui must be 8 bytes"));
    }
    if (app_key_buf.len != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("app_key must be 16 bytes"));
    }

    int timeout_s = (int)args[ARG_timeout].u_int;
    if (timeout_s <= 0) timeout_s = 30;

    lorawan_cmd_data_t cmd;
    cmd.cmd = CMD_JOIN_OTAA;
    memcpy(cmd.join_otaa.dev_eui,  dev_eui_buf.buf,  8);
    memcpy(cmd.join_otaa.join_eui, join_eui_buf.buf, 8);
    memcpy(cmd.join_otaa.app_key,  app_key_buf.buf,  16);

    // nwk_key: if provided use it (LoRaWAN 1.1); otherwise copy app_key (1.0.x behaviour).
    if (args[ARG_nwk_key].u_obj != mp_const_none) {
        mp_buffer_info_t nwk_key_buf;
        mp_get_buffer_raise(args[ARG_nwk_key].u_obj, &nwk_key_buf, MP_BUFFER_READ);
        if (nwk_key_buf.len != 16) {
            mp_raise_ValueError(MP_ERROR_TEXT("nwk_key must be 16 bytes"));
        }
        memcpy(cmd.join_otaa.nwk_key, nwk_key_buf.buf, 16);
    } else {
        memcpy(cmd.join_otaa.nwk_key, app_key_buf.buf, 16);
    }

    // Phase 1: send command, wait for MAC to accept it.
    send_cmd_wait(&cmd, EVT_COMPLETED);

    // Phase 2: wait for the join accept (or timeout).
    // s_otaa_active remains set until mlme_confirm signals EVT_JOIN_DONE.
    // On timeout, clear s_otaa_active so the retry loop stops.
    xEventGroupClearBits(s_events, EVT_JOIN_DONE);
    EventBits_t bits = xEventGroupWaitBits(s_events, EVT_JOIN_DONE,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS((uint32_t)timeout_s * 1000));

    if (!(bits & EVT_JOIN_DONE)) {
        s_otaa_active = false;  // stop retry loop
        mp_raise_OSError(MP_ETIMEDOUT);
    }

    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_join_otaa_obj, 1, lorawan_join_otaa);

static mp_obj_t lorawan_send(size_t n_args, const mp_obj_t *pos_args,
                              mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);

    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (!self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not joined"));
    }

    enum { ARG_data, ARG_port, ARG_confirmed, ARG_datarate };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data,      MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_port,      MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int  = 1} },
        { MP_QSTR_confirmed, MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = false} },
        // DR_0=SF12 .. DR_5=SF7; default DR_0 for maximum range on first uplinks
        { MP_QSTR_datarate,  MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int  = DR_0} },
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
    cmd.cmd           = CMD_TX;
    cmd.tx.len        = (uint8_t)buf.len;
    cmd.tx.port       = (uint8_t)args[ARG_port].u_int;
    cmd.tx.confirmed  = args[ARG_confirmed].u_bool;
    cmd.tx.datarate   = (uint8_t)args[ARG_datarate].u_int;
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
    // self->joined is set by join_abp, by mlme_confirm(MLME_JOIN, OK), and by nvram_restore.
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
                      mp_obj_new_int_from_uint(self->tx_counter));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_rx_counter),
                      mp_obj_new_int_from_uint(self->rx_counter));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_tx_time_on_air),
                      mp_obj_new_int_from_uint(self->tx_time_on_air));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_tx_ack),
                      mp_obj_new_bool(self->last_tx_ack));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_stats_obj, lorawan_stats);

// recv(timeout=10) -> (data, port, rssi, snr) or None
// Blocks until a downlink arrives or the timeout (seconds) expires.
// Use timeout=0 to poll without blocking.
static mp_obj_t lorawan_recv(size_t n_args, const mp_obj_t *pos_args,
                              mp_map_t *kw_args) {
    enum { ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_timeout, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = 10} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int timeout_s = (int)args[ARG_timeout].u_int;
    TickType_t ticks = (timeout_s > 0)
                       ? pdMS_TO_TICKS((uint32_t)timeout_s * 1000)
                       : 0;

    lorawan_rx_pkt_t pkt;
    if (xQueueReceive(s_rx_queue, &pkt, ticks) != pdTRUE) {
        return mp_const_none;
    }

    mp_obj_t items[4];
    items[0] = mp_obj_new_bytes(pkt.data, pkt.len);
    items[1] = mp_obj_new_int(pkt.port);
    items[2] = mp_obj_new_int(pkt.rssi);
    items[3] = mp_obj_new_int(pkt.snr);
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_recv_obj, 1, lorawan_recv);

// on_rx(callback) — register callback(data, port, rssi, snr) for incoming downlinks.
// Pass None to deregister. Do not use together with recv() on the same object.
static mp_obj_t lorawan_on_rx(mp_obj_t self_in, mp_obj_t cb) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_rx: callback must be callable or None"));
    }
    self->rx_callback = cb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_on_rx_obj, lorawan_on_rx);

// on_tx_done(callback) — register callback(success) for uplink completion.
// For confirmed uplinks: success=True means ACK received from network.
// For unconfirmed uplinks: success=True means frame was transmitted.
// Pass None to deregister.
static mp_obj_t lorawan_on_tx_done(mp_obj_t self_in, mp_obj_t cb) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_tx_done: callback must be callable or None"));
    }
    self->tx_callback = cb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_on_tx_done_obj, lorawan_on_tx_done);

// nvram_save() — persist MAC session to ESP32 NVS.
// Saves the full LoRaMacNvmData_t blob (DevAddr, session keys, FCnt, region
// params, ADR state) so that nvram_restore() can resume without re-joining.
static mp_obj_t lorawan_nvram_save(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_NVRAM_SAVE };
    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_NVRAM_OK | EVT_NVRAM_ERROR);
    if (bits & EVT_NVRAM_ERROR) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("nvram_save failed"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_nvram_save_obj, lorawan_nvram_save);

// nvram_restore() — restore MAC session from ESP32 NVS.
// After restore the device is marked joined and can send without re-joining.
// Raises OSError(ENODATA) if no saved session exists.
static mp_obj_t lorawan_nvram_restore(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_NVRAM_RESTORE };
    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_NVRAM_OK | EVT_NVRAM_ERROR);
    if (bits & EVT_NVRAM_ERROR) {
        mp_raise_OSError(MP_ENOENT);  // nothing saved yet, or NVS read error
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_nvram_restore_obj, lorawan_nvram_restore);

// datarate(dr=None) — getter/setter for MIB_CHANNELS_DATARATE.
// Getter returns the current DR (DR_0..DR_5).
// Setter sets the DR; only effective when ADR is off.
static mp_obj_t lorawan_datarate(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (n_args == 1) {
        return mp_obj_new_int(self->channels_datarate);
    }
    lorawan_cmd_data_t cmd;
    cmd.cmd = CMD_SET_PARAMS;
    cmd.set_params.type = 1;
    cmd.set_params.dr = (int8_t)mp_obj_get_int(args[1]);
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_datarate_obj, 1, 2, lorawan_datarate);

// adr(enabled=None) — getter/setter for MIB_ADR.
// Enabling ADR while a tx_power override is active clears the override:
// ADR adjusts the MAC TX power index, which must reach the radio for ADR to work.
static mp_obj_t lorawan_adr(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (n_args == 1) {
        return mp_obj_new_bool(self->adr_enabled);
    }
    bool enable = mp_obj_is_true(args[1]);
    if (enable && g_tx_power_hw_override != LORAWAN_TX_POWER_NO_OVERRIDE) {
        mp_printf(&mp_plat_print,
                  "lorawan: ADR enabled — clearing tx_power override (%d dBm)\n",
                  (int)g_tx_power_hw_override);
        g_tx_power_hw_override = LORAWAN_TX_POWER_NO_OVERRIDE;
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_SET_PARAMS };
    cmd.set_params.type = 0;
    cmd.set_params.adr  = enable;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_adr_obj, 1, 2, lorawan_adr);

// tx_power(dbm=None) — getter/setter for TX power in dBm EIRP.
// EU868 regulatory range: 2–16 dBm (step 2 dBm) via MAC index.
// Values beyond the region max bypass MAC validation via a hardware override
// (g_tx_power_hw_override). Hardware caps: SX1276 ≤ 20 dBm, SX1262 ≤ 22 dBm.
// Override and ADR are mutually exclusive — setting one disables the other.
static mp_obj_t lorawan_tx_power(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (n_args == 1) {
        // If override is active return actual hardware power, not the MAC index.
        if (g_tx_power_hw_override != LORAWAN_TX_POWER_NO_OVERRIDE) {
            return mp_obj_new_int((int)g_tx_power_hw_override);
        }
        return mp_obj_new_int(tx_power_to_dbm(self->channels_tx_power));
    }

    int    dbm     = (int)mp_obj_get_int(args[1]);
    int8_t idx     = dbm_to_tx_power(dbm);
    int    mac_max = tx_power_to_dbm(0); // region table max (EU868: 16 dBm)

    if (dbm > mac_max) {
        // Beyond region regulatory limit: activate hardware override.
        g_tx_power_hw_override = (int8_t)dbm;
        mp_printf(&mp_plat_print,
                  "lorawan: tx_power %d dBm — above region limit (%d dBm), user responsibility\n",
                  dbm, mac_max);
        // ADR adjusts the MAC index, which hardware would then ignore — disable it.
        if (self->adr_enabled) {
            mp_printf(&mp_plat_print,
                      "lorawan: ADR disabled (incompatible with tx_power override)\n");
            lorawan_cmd_data_t adr_cmd = { .cmd = CMD_SET_PARAMS };
            adr_cmd.set_params.type = 0;
            adr_cmd.set_params.adr  = false;
            send_cmd_wait(&adr_cmd, EVT_COMPLETED);
        }
        idx = 0; // MAC stays at region max
    } else {
        // Within regulatory limits: clear any previous override.
        g_tx_power_hw_override = LORAWAN_TX_POWER_NO_OVERRIDE;
    }

    lorawan_cmd_data_t cmd = { .cmd = CMD_SET_PARAMS };
    cmd.set_params.type     = 2;
    cmd.set_params.tx_power = idx;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_tx_power_obj, 1, 2, lorawan_tx_power);

// antenna_gain(gain=None) — getter/setter for MIB_ANTENNA_GAIN.
// Getter returns the current antenna gain in dBi as a float.
// Setter updates both MIB_ANTENNA_GAIN and MIB_DEFAULT_ANTENNA_GAIN so a
// subsequent ResetMacParameters doesn't restore the region default.
static mp_obj_t lorawan_antenna_gain(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (n_args == 1) {
        return mp_obj_new_float(self->antenna_gain);
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_SET_PARAMS };
    cmd.set_params.type         = 3;
    cmd.set_params.antenna_gain = mp_obj_get_float(args[1]);
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_antenna_gain_obj, 1, 2, lorawan_antenna_gain);

// request_class(cls) — request a switch to CLASS_A or CLASS_C.
// CLASS_B is not yet supported (requires beacon acquisition, Session 13).
// LoRaMAC-node v4.7.0 performs A <-> C transitions synchronously via
// MIB_DEVICE_CLASS; the on_class_change callback fires after success.
static mp_obj_t lorawan_request_class(mp_obj_t self_in, mp_obj_t cls_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    int cls = mp_obj_get_int(cls_in);
    if (cls != CLASS_A && cls != CLASS_B && cls != CLASS_C) {
        mp_raise_ValueError(MP_ERROR_TEXT("class must be CLASS_A, CLASS_B or CLASS_C"));
    }
    if (cls == CLASS_B) {
        mp_raise_NotImplementedError(MP_ERROR_TEXT("CLASS_B not implemented"));
    }

    lorawan_cmd_data_t cmd;
    cmd.cmd = CMD_SET_CLASS;
    cmd.device_class = (uint8_t)cls;
    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_CLASS_OK | EVT_CLASS_ERROR);
    if (bits & EVT_CLASS_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("class switch failed"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_request_class_obj, lorawan_request_class);

// device_class() — returns the current device class (CLASS_A, CLASS_B, CLASS_C).
static mp_obj_t lorawan_device_class(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int((int)self->device_class);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_device_class_obj, lorawan_device_class);

// on_class_change(callback) — register callback(new_class) for class transitions.
// Pass None to deregister.
static mp_obj_t lorawan_on_class_change(mp_obj_t self_in, mp_obj_t cb) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_class_change: callback must be callable or None"));
    }
    self->class_callback = cb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_on_class_change_obj, lorawan_on_class_change);

static const mp_rom_map_elem_t lorawan_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_join_abp),       MP_ROM_PTR(&lorawan_join_abp_obj) },
    { MP_ROM_QSTR(MP_QSTR_join_otaa),      MP_ROM_PTR(&lorawan_join_otaa_obj) },
    { MP_ROM_QSTR(MP_QSTR_send),           MP_ROM_PTR(&lorawan_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv),           MP_ROM_PTR(&lorawan_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_joined),         MP_ROM_PTR(&lorawan_joined_obj) },
    { MP_ROM_QSTR(MP_QSTR_stats),          MP_ROM_PTR(&lorawan_stats_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_rx),          MP_ROM_PTR(&lorawan_on_rx_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_tx_done),     MP_ROM_PTR(&lorawan_on_tx_done_obj) },
    { MP_ROM_QSTR(MP_QSTR_nvram_save),     MP_ROM_PTR(&lorawan_nvram_save_obj) },
    { MP_ROM_QSTR(MP_QSTR_nvram_restore),  MP_ROM_PTR(&lorawan_nvram_restore_obj) },
    { MP_ROM_QSTR(MP_QSTR_datarate),       MP_ROM_PTR(&lorawan_datarate_obj) },
    { MP_ROM_QSTR(MP_QSTR_adr),            MP_ROM_PTR(&lorawan_adr_obj) },
    { MP_ROM_QSTR(MP_QSTR_tx_power),       MP_ROM_PTR(&lorawan_tx_power_obj) },
    { MP_ROM_QSTR(MP_QSTR_antenna_gain),   MP_ROM_PTR(&lorawan_antenna_gain_obj) },
    { MP_ROM_QSTR(MP_QSTR_request_class),  MP_ROM_PTR(&lorawan_request_class_obj) },
    { MP_ROM_QSTR(MP_QSTR_device_class),   MP_ROM_PTR(&lorawan_device_class_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_class_change), MP_ROM_PTR(&lorawan_on_class_change_obj) },
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
    return mp_obj_new_str("0.8.0", 5);
}
static MP_DEFINE_CONST_FUN_OBJ_0(lorawan_version_obj, lorawan_version);

// lorawan.test_hal() — preserved from Phase 3, exercises HAL without the MAC stack
static volatile bool s_test_timer_fired = false;

static void test_timer_cb(void *ctx) {
    (void)ctx;
    s_test_timer_fired = true;
}

static mp_obj_t lorawan_test_hal(void) {
    uint8_t reg42     = lorawan_radio_probe_reg42();
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
    // LoRaWAN protocol version constants
    { MP_ROM_QSTR(MP_QSTR_V1_0_4),    MP_ROM_INT(LORAWAN_V1_0_4) },
    { MP_ROM_QSTR(MP_QSTR_V1_1),      MP_ROM_INT(LORAWAN_V1_1) },
    // Data rate constants (EU868): DR_0=SF12/BW125 .. DR_5=SF7/BW125
    { MP_ROM_QSTR(MP_QSTR_DR_0),      MP_ROM_INT(DR_0) },
    { MP_ROM_QSTR(MP_QSTR_DR_1),      MP_ROM_INT(DR_1) },
    { MP_ROM_QSTR(MP_QSTR_DR_2),      MP_ROM_INT(DR_2) },
    { MP_ROM_QSTR(MP_QSTR_DR_3),      MP_ROM_INT(DR_3) },
    { MP_ROM_QSTR(MP_QSTR_DR_4),      MP_ROM_INT(DR_4) },
    { MP_ROM_QSTR(MP_QSTR_DR_5),      MP_ROM_INT(DR_5) },
};
static MP_DEFINE_CONST_DICT(lorawan_module_globals, lorawan_module_globals_table);

const mp_obj_module_t lorawan_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lorawan_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lorawan, lorawan_module);
