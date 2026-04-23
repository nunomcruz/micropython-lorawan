// modlorawan.c — LoRaWAN Python bindings
// Phase 4, Sessions 7-10: lorawan_obj_t, __init__, join_abp, join_otaa, send, stats,
//                         recv, on_rx, on_tx_done, confirmed uplink ACK tracking,
//                         nvram_save/restore (ESP32 NVS via MIB_NVM_CTXS),
//                         datarate/adr/tx_power getters+setters.
// Phase 5, Session 11:   runtime pin config; lorawan_version (1.0.4/1.1), rx2_dr/freq,
//                         tx_power kwargs on __init__; nwk_key kwarg on join_otaa;
//                         request_class / device_class / on_class_change (Class A <-> C);
//                         antenna_gain kwarg + getter/setter (MIB_ANTENNA_GAIN).
// Phase 5, Session 16:   lifecycle — deinit() / __del__ / __enter__ / __exit__.
//                         Finaliser-backed allocation so the GC sweep during
//                         soft-reset tears down the FreeRTOS task + radio HAL.
// FreeRTOS task owns all MAC calls; Python thread communicates via queue + event group.

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
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
#include "LoRaMacTest.h"
#include "region/RegionEU868.h"
#include "systime.h"
#include "lmhandler_shim.h"

// Declared in hal/esp32_board.c. Not in loramac-node's board.h — the MAC's
// GetTemperatureLevel callback is plugged in via s_mac_callbacks directly.
float BoardGetTemperatureLevel(void);
void BoardDeInitMcu(void);

// Radio HAL teardown. Declared in loramac-node/src/boards/sx*-board.h, but
// those headers collide with each other so we redeclare the two entry
// points we need here (same pattern as BoardGetTemperatureLevel above).
void SX1276IoDeInit(void);
void SX126xIoDeInit(void);

// HAL teardown helpers: stop/delete the shared esp_timer backing LoRaMAC's
// timer list, and release the SX126x recursive SPI mutex. Declared in
// hal/esp32_timer.c and hal/sx126x_board.c respectively.
void lorawan_timer_deinit(void);
void sx126x_spi_mutex_deinit(void);

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

// Beacon state codes surfaced to Python via on_beacon(state, info).
// Mapped from MLME_BEACON / MLME_BEACON_LOST indications and the
// MLME_BEACON_ACQUISITION confirm outcome.
#define LW_BEACON_ACQUISITION_OK    1  // acquisition confirmed, first lock
#define LW_BEACON_ACQUISITION_FAIL  2  // acquisition search timed out
#define LW_BEACON_LOCKED            3  // beacon received during normal operation
#define LW_BEACON_NOT_FOUND         4  // expected beacon not received this period
#define LW_BEACON_LOST              5  // beacon loss exceeded; MAC reverts to Class A

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
    CMD_REQUEST_DEVICE_TIME,
    CMD_REQUEST_LINK_CHECK,
    CMD_REQUEST_REJOIN,
    CMD_CLOCK_SYNC_ENABLE,
    CMD_CLOCK_SYNC_REQUEST,
    CMD_PING_SLOT_PERIODICITY,
    CMD_MC_ADD,
    CMD_MC_RX_PARAMS,
    CMD_MC_REMOVE,
    CMD_REMOTE_MCAST_ENABLE,
    CMD_FRAGMENTATION_ENABLE,
    CMD_DEINIT,
    CMD_QUERY,
} lorawan_cmd_t;

// Query sub-types for CMD_QUERY. Result is written to s_query_* statics
// before EVT_COMPLETED fires; the Python caller reads them after send_cmd_wait.
#define LW_QUERY_DEV_ADDR        0
#define LW_QUERY_MAX_PAYLOAD_LEN 1

typedef struct {
    uint32_t dev_addr;
    uint8_t  nwk_s_key[16];
    uint8_t  app_s_key[16];
} cmd_join_abp_t;

typedef struct {
    uint8_t  group;            // 0..3
    uint32_t addr;
    uint8_t  mc_nwk_s_key[16];
    uint8_t  mc_app_s_key[16];
    uint32_t f_count_min;
    uint32_t f_count_max;
} cmd_mc_add_t;

typedef struct {
    uint8_t  group;            // 0..3
    uint8_t  device_class;     // CLASS_B or CLASS_C
    uint32_t frequency;        // Hz
    int8_t   datarate;         // DR_0..DR_5
    uint16_t periodicity;      // Class B only: 2^N s
} cmd_mc_rx_params_t;

typedef struct {
    uint32_t buffer_size;
} cmd_frag_enable_t;

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

// set_params.type: 0=ADR, 1=DR, 2=TX_POWER, 3=ANTENNA_GAIN, 4=DUTY_CYCLE
typedef struct {
    uint8_t type;
    union {
        bool   adr;
        int8_t dr;
        int8_t tx_power;
        float  antenna_gain;
        bool   duty_cycle;
    };
} cmd_set_params_t;

typedef struct {
    lorawan_cmd_t cmd;
    union {
        cmd_join_abp_t  join_abp;
        cmd_join_otaa_t join_otaa;
        cmd_tx_t        tx;
        cmd_set_params_t set_params;
        uint8_t         device_class;       // CMD_SET_CLASS: DeviceClass_t value
        uint8_t         rejoin_type;        // CMD_REQUEST_REJOIN: 0, 1 or 2
        uint8_t         ping_periodicity;   // CMD_PING_SLOT_PERIODICITY: 0..7
        cmd_mc_add_t    mc_add;
        cmd_mc_rx_params_t mc_rx_params;
        uint8_t         mc_group;           // CMD_MC_REMOVE: 0..3
        cmd_frag_enable_t frag_enable;
        uint8_t         query_type;         // CMD_QUERY: LW_QUERY_*
    };
} lorawan_cmd_data_t;

// ---- Received packet ----

typedef struct {
    uint8_t  data[LW_PAYLOAD_MAX];
    uint8_t  len;
    uint8_t  port;
    int16_t  rssi;
    int8_t   snr;
    bool     multicast;
} lorawan_rx_pkt_t;

// Local snapshot of multicast group state. The MAC holds the authoritative
// copy in Nvm.MacGroup2.MulticastChannelList but exposes no getter, so we
// mirror the key fields here for multicast_list() without touching internals.
typedef struct {
    bool     active;           // true once multicast_add succeeds
    bool     is_remote;        // from IsRemotelySetup
    bool     rx_params_set;    // true once multicast_rx_params succeeds
    uint32_t addr;
    uint32_t f_count_min;
    uint32_t f_count_max;
    DeviceClass_t rx_class;    // CLASS_B or CLASS_C (only valid if rx_params_set)
    uint32_t rx_frequency;
    int8_t   rx_datarate;
    uint16_t rx_periodicity;   // Class B only
} lorawan_mc_group_t;

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
    mp_obj_t        time_sync_callback;
    // Last uplink stats
    uint32_t        tx_counter;
    uint32_t        tx_time_on_air;
    bool            last_tx_ack;    // confirmed uplink: true if network ACKed
    // Snapshot of the parameters the MAC actually used for the most recent
    // uplink, captured in mcps_confirm. Distinct from the live datarate() /
    // tx_power() getters: those report the *next* TX values after any ADR
    // adjustment in the same confirm, which can mask what was just sent.
    // Zero before the first TX (mirrors the existing tx_counter convention).
    int8_t          last_tx_dr;
    int8_t          last_tx_power;  // MAC index (0=region max); converted to dBm in stats()
    uint32_t        last_tx_freq;   // Hz; resolved from McpsConfirm.Channel via MIB_CHANNELS
    // Last downlink stats
    uint32_t        rx_counter;
    int16_t         rssi;
    int8_t          snr;
    // Cached MAC parameters (updated on init and after each TX when ADR is on)
    int8_t          channels_datarate;
    bool            adr_enabled;
    int8_t          channels_tx_power; // TX_POWER index (0=max); convert to/from dBm at API boundary
    // Duty-cycle state. duty_cycle_enabled mirrors Nvm.MacGroup2.DutyCycleOn so
    // the getter is O(1) (no MIB round-trip). last_dc_wait_ms is the wait time
    // returned by the most recent McpsRequest.ReqReturn.DutyCycleWaitTime, with
    // last_dc_query_us the esp_timer_get_time() at the moment we read it; their
    // difference is what time_until_tx() returns (clamped to >= 0).
    bool            duty_cycle_enabled;
    uint32_t        last_dc_wait_ms;
    int64_t         last_dc_query_us;
    // Antenna gain in dBi. MAC subtracts this from EIRP when computing radio TX power:
    //   radioTxPower = floor(maxEirp - txPowerIndex*2 - antennaGain)
    // EU868 region default is 2.15 dBi; we default to 0.0 so the radio emits full EIRP
    // unless the user explicitly declares a gain.
    float           antenna_gain;
    // Network time sync — populated by MLME_DEVICE_TIME confirm (DeviceTimeAns).
    // network_time_gps is GPS epoch seconds captured at the moment the ans was
    // processed; time_synced becomes true on first successful sync and stays true.
    bool            time_synced;
    uint32_t        network_time_gps;
    // Link check — populated by MLME_LINK_CHECK confirm (LinkCheckAns).
    // link_check_received stays true after the first successful answer; each
    // new answer overwrites margin/gw_count.
    bool            link_check_received;
    uint8_t         link_check_margin;
    uint8_t         link_check_gw_count;
    // Clock Sync (LmhpClockSync, port 202). clock_sync_enabled flips true once
    // the package is registered via clock_sync_enable(). The time fields above
    // (time_synced / network_time_gps) are shared between the MAC DeviceTimeReq
    // path and the application-layer AppTimeReq path, so both update the same
    // snapshot.
    bool            clock_sync_enabled;
    // Class B — beacon + ping slot state, populated by the MLME handlers.
    mp_obj_t        beacon_callback;           // fires on every MLME_BEACON indication
    uint8_t         ping_slot_periodicity;     // 0..7, period = 2^N seconds
    // Snapshot of the most recent BeaconInfo_t delivered by the MAC.
    // beacon_info_valid flips true on first BEACON_LOCKED and stays true; the
    // rest of the fields may be overwritten by later indications (LOST clears them).
    bool            beacon_info_valid;
    uint8_t         beacon_last_state;         // one of LW_BEACON_* codes; 0 = none yet
    uint32_t        beacon_time_seconds;       // GPS-epoch seconds of last locked beacon
    uint32_t        beacon_freq;
    uint8_t         beacon_datarate;
    int16_t         beacon_rssi;
    int8_t          beacon_snr;
    uint8_t         beacon_gw_info_desc;
    uint8_t         beacon_gw_info[6];
    // Multicast — up to 4 local groups (LORAMAC_MAX_MC_CTX). The MAC holds the
    // authoritative state and crypto material; we mirror only the metadata
    // Python needs to answer multicast_list().
    lorawan_mc_group_t mc_groups[4];
    // Remote Multicast Setup (LmhpRemoteMcastSetup, port 200). Flips true once
    // the package has been registered via remote_multicast_enable().
    bool            remote_mcast_enabled;
    // Fragmentation (LmhpFragmentation, port 201 — FUOTA base).
    bool            fragmentation_enabled;
    uint32_t        fragmentation_buffer_size; // size of the decoder buffer
    uint32_t        fragmentation_last_size;   // size reported by on_done
    int32_t         fragmentation_last_status; // status reported by on_done
    mp_obj_t        fragmentation_progress_callback;
    mp_obj_t        fragmentation_done_callback;
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

// Class B orchestration flags — set from MLME callback context, acted on from the
// lorawan_task main loop so LoRaMacMlmeRequest / LoRaMacMibSetRequestConfirm are
// only called outside of a MAC callback (same pattern as the OTAA retry flag).
static volatile bool    s_classb_active;                // request_class(CLASS_B) has been issued
static volatile bool    s_classb_need_ping_slot_req;    // BEACON_ACQUISITION OK → send PingSlotInfoReq
static volatile bool    s_classb_need_switch_class;     // PING_SLOT_INFO OK → set MIB_DEVICE_CLASS
static volatile bool    s_classb_need_device_time_req;  // BEACON_ACQUISITION FAIL → re-sync time
static volatile uint8_t s_classb_periodicity;           // 0..7, passed to MLME_PING_SLOT_INFO
static uint8_t       s_otaa_dev_eui[8];
static uint8_t       s_otaa_join_eui[8];
static uint8_t       s_otaa_app_key[16];
static uint8_t       s_otaa_nwk_key[16]; // 1.0.x: same as app_key; 1.1: separate root key

// CMD_QUERY result statics. Written by the LoRaWAN task before EVT_COMPLETED;
// read by the Python thread after send_cmd_wait returns. Safe because the
// caller is blocked on the event group while the task fills these.
static uint32_t      s_query_dev_addr;
static uint8_t       s_query_max_payload_len;

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
    // (data, port, rssi, snr, multicast) — multicast flag added in Session 14.
    mp_obj_t items[5];
    items[0] = mp_obj_new_bytes(pkt.data, pkt.len);
    items[1] = mp_obj_new_int(pkt.port);
    items[2] = mp_obj_new_int(pkt.rssi);
    items[3] = mp_obj_new_int(pkt.snr);
    items[4] = mp_obj_new_bool(pkt.multicast);
    mp_call_function_1(s_lora_obj->rx_callback, mp_obj_new_tuple(5, items));
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

// Called when a DeviceTimeAns has been processed and the MAC system time
// has been updated. Passes the GPS epoch seconds (uint32) to the callback.
// We pass mp_const_none as the scheduler arg and read the timestamp from
// the object here — allocating a Python int in the LoRaWAN task context
// (no GIL) is not safe, and GPS epoch in 2026 exceeds the 31-bit small-int
// range so MP_OBJ_NEW_SMALL_INT would overflow.
static mp_obj_t lorawan_time_sync_trampoline(mp_obj_t arg) {
    (void)arg;
    if (!s_lora_obj || s_lora_obj->time_sync_callback == mp_const_none) {
        return mp_const_none;
    }
    mp_obj_t t = mp_obj_new_int_from_uint(s_lora_obj->network_time_gps);
    mp_call_function_1(s_lora_obj->time_sync_callback, t);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_time_sync_trampoline_obj, lorawan_time_sync_trampoline);

// Forward declaration — the builder is defined below (it allocates dict/bytes
// objects so it can only run in VM context).
static mp_obj_t lorawan_beacon_info_dict(lorawan_obj_t *self);

// Called on every MLME_BEACON / MLME_BEACON_LOST / MLME_BEACON_ACQUISITION
// event. The scheduler arg carries the LW_BEACON_* state code (small int);
// info is read from the object snapshot updated in the MLME handler.
static mp_obj_t lorawan_beacon_trampoline(mp_obj_t arg) {
    if (!s_lora_obj || s_lora_obj->beacon_callback == mp_const_none) {
        return mp_const_none;
    }
    mp_obj_t info = s_lora_obj->beacon_info_valid
                    ? lorawan_beacon_info_dict(s_lora_obj)
                    : mp_const_none;
    mp_obj_t items[2] = { arg, info };
    mp_call_function_1(s_lora_obj->beacon_callback, mp_obj_new_tuple(2, items));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_beacon_trampoline_obj, lorawan_beacon_trampoline);

// Fragmentation progress — a full 4-tuple (counter, total, size, lost) would
// overflow the scheduler arg, so we snapshot the values on a static buffer
// (single-producer: the LoRaWAN task) and the trampoline reads them in VM
// context. Last-one-wins semantics: if progress fires faster than Python can
// dispatch we only keep the newest snapshot. Typical case: slow downlinks
// every few seconds, not a concern in practice.
static volatile uint16_t s_frag_progress_counter;
static volatile uint16_t s_frag_progress_nb;
static volatile uint8_t  s_frag_progress_size;
static volatile uint16_t s_frag_progress_lost;

static mp_obj_t lorawan_frag_progress_trampoline(mp_obj_t arg) {
    (void)arg;
    if (!s_lora_obj || s_lora_obj->fragmentation_progress_callback == mp_const_none) {
        return mp_const_none;
    }
    mp_obj_t items[4];
    items[0] = mp_obj_new_int(s_frag_progress_counter);
    items[1] = mp_obj_new_int(s_frag_progress_nb);
    items[2] = mp_obj_new_int(s_frag_progress_size);
    items[3] = mp_obj_new_int(s_frag_progress_lost);
    mp_call_function_1(s_lora_obj->fragmentation_progress_callback,
                       mp_obj_new_tuple(4, items));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_frag_progress_trampoline_obj,
                                  lorawan_frag_progress_trampoline);

static mp_obj_t lorawan_frag_done_trampoline(mp_obj_t arg) {
    (void)arg;
    if (!s_lora_obj || s_lora_obj->fragmentation_done_callback == mp_const_none) {
        return mp_const_none;
    }
    // status=0 → success; >0 → number of missing fragments; -1 → ongoing; -2 → not started.
    // size is the file size after reassembly; buffer is read back via
    // lorawan_fragmentation_buffer() — exposed to Python via the
    // fragmentation_data() method.
    mp_obj_t items[2];
    items[0] = mp_obj_new_int(s_lora_obj->fragmentation_last_status);
    items[1] = mp_obj_new_int_from_uint(s_lora_obj->fragmentation_last_size);
    mp_call_function_1(s_lora_obj->fragmentation_done_callback,
                       mp_obj_new_tuple(2, items));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_frag_done_trampoline_obj,
                                  lorawan_frag_done_trampoline);

// Called by lmhandler_shim.c from LmhpFragmentation's OnProgress callback.
// Running in LoRaWAN task context (no GIL) so we can only snapshot values
// and schedule the Python trampoline — no allocations here.
void lorawan_on_fragmentation_progress(uint16_t frag_counter,
                                        uint16_t frag_nb,
                                        uint8_t  frag_size,
                                        uint16_t frag_nb_lost) {
    if (!s_lora_obj) return;
    s_frag_progress_counter = frag_counter;
    s_frag_progress_nb      = frag_nb;
    s_frag_progress_size    = frag_size;
    s_frag_progress_lost    = frag_nb_lost;
    if (s_lora_obj->fragmentation_progress_callback != mp_const_none) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_frag_progress_trampoline_obj),
                          mp_const_none);
    }
}

void lorawan_on_fragmentation_done(int32_t status, uint32_t size) {
    if (!s_lora_obj) return;
    s_lora_obj->fragmentation_last_status = status;
    s_lora_obj->fragmentation_last_size   = size;
    esp_rom_printf("lorawan: fragmentation done status=%d size=%lu\n",
                   (int)status, (unsigned long)size);
    if (s_lora_obj->fragmentation_done_callback != mp_const_none) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_frag_done_trampoline_obj),
                          mp_const_none);
    }
}

// Called by lmhandler_shim.c whenever LmhpClockSync has applied a correction
// from an AppTimeAns (port 202). SysTime has already been updated, so we just
// snapshot the GPS epoch onto the active object and fire the user callback
// via the existing time-sync trampoline.
void lorawan_on_sys_time_update(bool is_synchronized, int32_t correction) {
    (void)is_synchronized;
    (void)correction;
    if (!s_lora_obj) return;

    SysTime_t now = SysTimeGet();
    uint32_t gps_epoch = (now.Seconds >= UNIX_GPS_EPOCH_OFFSET)
                         ? (now.Seconds - UNIX_GPS_EPOCH_OFFSET) : 0;
    s_lora_obj->time_synced      = true;
    s_lora_obj->network_time_gps = gps_epoch;

    esp_rom_printf("lorawan: ClockSync AppTimeAns OK, GPS epoch=%lu (corr=%ld)\n",
                   (unsigned long)gps_epoch, (long)correction);

    if (s_lora_obj->time_sync_callback != mp_const_none) {
        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_time_sync_trampoline_obj),
                          mp_const_none);
    }
}

// ---- LoRaMAC MAC-layer callbacks (run in the LoRaWAN task context) ----

static void mcps_confirm(McpsConfirm_t *c) {
    if (!c) return;

    // Fan out to any registered LmHandler package (Clock Sync etc.) before
    // touching object state — the package needs confirmation of its own
    // port-202 uplinks to revert ADR / NbTrans / DR.
    lorawan_packages_on_mcps_confirm(c);

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

            // Snapshot the params actually used for this TX. Datarate / TxPower
            // come straight off the confirm; the Channel field is an index into
            // the region channel list — resolve it to Hz via MIB_CHANNELS so
            // Python sees a frequency, not an opaque slot number.
            s_lora_obj->last_tx_dr    = (int8_t)c->Datarate;
            s_lora_obj->last_tx_power = c->TxPower;
            s_lora_obj->last_tx_freq  = 0;
            MibRequestConfirm_t ch_mib;
            ch_mib.Type = MIB_CHANNELS;
            if (LoRaMacMibGetRequestConfirm(&ch_mib) == LORAMAC_STATUS_OK
                && ch_mib.Param.ChannelList != NULL
                && c->Channel < 16) {
                s_lora_obj->last_tx_freq = ch_mib.Param.ChannelList[c->Channel].Frequency;
            }

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

    // Fan out to any registered LmHandler package first. LmhpClockSync only
    // consumes frames on port 202 and answers them on the same port, so this
    // consumes packages' traffic before the port-224-filter below.
    lorawan_packages_on_mcps_indication(ind);

    if (s_lora_obj) {
        s_lora_obj->rssi       = ind->Rssi;
        s_lora_obj->snr        = ind->Snr;
        s_lora_obj->rx_counter = ind->DownLinkCounter;
    }

    if (!ind->RxData || ind->BufferSize == 0) return;
    if (ind->Port == 0 || ind->Port >= 224) return;

    lorawan_rx_pkt_t pkt;
    pkt.len       = (ind->BufferSize <= LW_PAYLOAD_MAX) ? (uint8_t)ind->BufferSize : LW_PAYLOAD_MAX;
    pkt.port      = ind->Port;
    pkt.rssi      = ind->Rssi;
    pkt.snr       = ind->Snr;
    pkt.multicast = (ind->Multicast != 0);
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

    switch (c->MlmeRequest) {
    case MLME_JOIN:
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
        break;

    case MLME_DEVICE_TIME:
        // DeviceTimeAns: the MAC has already called SysTimeSet() with the
        // network time (Unix epoch, computed from the GPS-epoch payload plus
        // UNIX_GPS_EPOCH_OFFSET).  SysTimeGet() now returns that value minus
        // any milliseconds since the RX window, so the synced time is
        // directly usable as GPS epoch seconds (SysTimeGet - offset).
        if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
            SysTime_t now = SysTimeGet();
            uint32_t gps_epoch = (now.Seconds >= UNIX_GPS_EPOCH_OFFSET)
                                 ? (now.Seconds - UNIX_GPS_EPOCH_OFFSET) : 0;
            if (s_lora_obj) {
                s_lora_obj->time_synced      = true;
                s_lora_obj->network_time_gps = gps_epoch;
                if (s_lora_obj->time_sync_callback != mp_const_none) {
                    mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_time_sync_trampoline_obj),
                                      mp_const_none);
                }
            }
            esp_rom_printf("lorawan: DeviceTimeAns OK, GPS epoch=%lu\n",
                           (unsigned long)gps_epoch);
        } else {
            esp_rom_printf("lorawan: MLME_DEVICE_TIME failed (status=%d)\n",
                           (int)c->Status);
        }
        break;

    case MLME_LINK_CHECK:
        // LinkCheckAns: the MAC extracts DemodMargin (link margin in dB from
        // the gateway with the best SNR) and NbGateways (how many gateways
        // received the last uplink carrying the LinkCheckReq). We cache both
        // on the object so Python can retrieve them via link_check().
        if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
            if (s_lora_obj) {
                s_lora_obj->link_check_received = true;
                s_lora_obj->link_check_margin   = c->DemodMargin;
                s_lora_obj->link_check_gw_count = c->NbGateways;
            }
            esp_rom_printf("lorawan: LinkCheckAns margin=%d gw_count=%d\n",
                           (int)c->DemodMargin, (int)c->NbGateways);
        } else {
            esp_rom_printf("lorawan: MLME_LINK_CHECK failed (status=%d)\n",
                           (int)c->Status);
        }
        break;

    case MLME_BEACON_ACQUISITION:
        // Beacon acquisition outcome. On success the MAC has locked onto a
        // beacon and `Ctx.BeaconCtx.Ctrl.BeaconMode` is now 1 — the next step
        // (PingSlotInfoReq) can proceed. On failure we trigger a fresh
        // DeviceTimeReq so beacon timing gets re-synced; the class-B state
        // machine then re-issues MLME_BEACON_ACQUISITION on the next cycle.
        if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
            esp_rom_printf("lorawan: beacon acquired\n");
            if (s_classb_active) {
                s_classb_need_ping_slot_req = true;
            }
            if (s_lora_obj && s_lora_obj->beacon_callback != mp_const_none) {
                mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_beacon_trampoline_obj),
                                  MP_OBJ_NEW_SMALL_INT(LW_BEACON_ACQUISITION_OK));
            }
        } else {
            esp_rom_printf("lorawan: beacon acquisition failed (status=%d)\n",
                           (int)c->Status);
            if (s_classb_active) {
                s_classb_need_device_time_req = true;
            }
            if (s_lora_obj && s_lora_obj->beacon_callback != mp_const_none) {
                mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_beacon_trampoline_obj),
                                  MP_OBJ_NEW_SMALL_INT(LW_BEACON_ACQUISITION_FAIL));
            }
        }
        break;

    case MLME_PING_SLOT_INFO:
        // PingSlotInfoAns arrived. On success the MAC has set
        // `PingSlotCtx.Ctrl.Assigned = 1` and the CLASS_A -> CLASS_B MIB switch
        // will now be accepted. On failure we re-issue the PingSlotInfoReq
        // from the task loop (piggy-back + empty uplink, same as LmHandler).
        if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
            esp_rom_printf("lorawan: PingSlotInfoAns OK\n");
            if (s_classb_active) {
                s_classb_need_switch_class = true;
            }
        } else {
            esp_rom_printf("lorawan: PingSlotInfoAns failed (status=%d), retrying\n",
                           (int)c->Status);
            if (s_classb_active) {
                s_classb_need_ping_slot_req = true;
            }
        }
        break;

    case MLME_REJOIN_0:
    case MLME_REJOIN_1:
    case MLME_REJOIN_2:
        // ReJoin-request was transmitted. For types 0 and 2 the network
        // answer is optional; for type 1 (periodic, LoRaWAN 1.1) the
        // network may reply with a Join-Accept that re-derives session
        // keys. The MAC handles key re-derivation internally; here we
        // just log the outcome.
        if (c->Status == LORAMAC_EVENT_INFO_STATUS_OK) {
            esp_rom_printf("lorawan: ReJoin type %d accepted\n",
                           (int)(c->MlmeRequest - MLME_REJOIN_0));
        } else {
            esp_rom_printf("lorawan: ReJoin type %d failed (status=%d)\n",
                           (int)(c->MlmeRequest - MLME_REJOIN_0),
                           (int)c->Status);
        }
        break;

    default:
        break;
    }
}

static void mlme_indication(MlmeIndication_t *ind) {
    if (!ind || !s_lora_obj) return;

    switch (ind->MlmeIndication) {
    case MLME_BEACON: {
        uint8_t state;
        if (ind->Status == LORAMAC_EVENT_INFO_STATUS_BEACON_LOCKED) {
            // Real beacon received — snapshot the BeaconInfo_t fields that
            // Python may want to inspect (time in GPS seconds, gateway-specific
            // bytes) so the trampoline can build the info dict in VM context.
            SysTime_t bt = ind->BeaconInfo.Time;
            s_lora_obj->beacon_info_valid   = true;
            s_lora_obj->beacon_time_seconds = bt.Seconds;
            s_lora_obj->beacon_freq         = ind->BeaconInfo.Frequency;
            s_lora_obj->beacon_datarate     = ind->BeaconInfo.Datarate;
            s_lora_obj->beacon_rssi         = ind->BeaconInfo.Rssi;
            s_lora_obj->beacon_snr          = ind->BeaconInfo.Snr;
            s_lora_obj->beacon_gw_info_desc = ind->BeaconInfo.GwSpecific.InfoDesc;
            memcpy(s_lora_obj->beacon_gw_info,
                   ind->BeaconInfo.GwSpecific.Info, 6);
            state = LW_BEACON_LOCKED;
            esp_rom_printf("lorawan: beacon locked rssi=%d snr=%d freq=%lu\n",
                           (int)ind->BeaconInfo.Rssi, (int)ind->BeaconInfo.Snr,
                           (unsigned long)ind->BeaconInfo.Frequency);
        } else {
            state = LW_BEACON_NOT_FOUND;
            esp_rom_printf("lorawan: beacon not found this period\n");
        }
        s_lora_obj->beacon_last_state = state;
        if (s_lora_obj->beacon_callback != mp_const_none) {
            mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_beacon_trampoline_obj),
                              MP_OBJ_NEW_SMALL_INT(state));
        }
        break;
    }

    case MLME_BEACON_LOST: {
        // Beacon loss has exceeded MAX_BEACON_LESS_PERIOD (2h). The MAC's
        // class-B machinery already aborted its timers; we force MIB_DEVICE_CLASS
        // back to CLASS_A (this also halts beaconing inside LoRaMacClassB) so
        // the node returns to the normal Class A RX windows.
        esp_rom_printf("lorawan: beacon lost, reverting to Class A\n");
        MibRequestConfirm_t mib;
        mib.Type = MIB_DEVICE_CLASS;
        mib.Param.Class = CLASS_A;
        LoRaMacMibSetRequestConfirm(&mib);

        s_classb_active             = false;
        s_classb_need_ping_slot_req = false;
        s_classb_need_switch_class  = false;

        s_lora_obj->beacon_last_state = LW_BEACON_LOST;
        s_lora_obj->beacon_info_valid = false;
        s_lora_obj->device_class      = CLASS_A;

        if (s_lora_obj->beacon_callback != mp_const_none) {
            mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_beacon_trampoline_obj),
                              MP_OBJ_NEW_SMALL_INT(LW_BEACON_LOST));
        }
        if (s_lora_obj->class_callback != mp_const_none) {
            mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_class_trampoline_obj),
                              MP_OBJ_NEW_SMALL_INT((mp_int_t)CLASS_A));
        }
        break;
    }

    case MLME_REVERT_JOIN:
        // LoRaWAN 1.1: the MAC did not receive a RekeyConf after a Join
        // Accept, so it has invalidated the session and requires the upper
        // layer to restart the join procedure. We mark the device as no
        // longer joined so send()/request_class() raise cleanly; the user
        // is expected to call join_otaa() again.
        esp_rom_printf("lorawan: MLME_REVERT_JOIN — session invalidated, rejoin required\n");
        s_lora_obj->joined = false;
        break;

    default:
        break;
    }
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

                s_mac_callbacks.GetBatteryLevel     = BoardGetBatteryLevel;
                s_mac_callbacks.GetTemperatureLevel = BoardGetTemperatureLevel;
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

                // RX2 window: each field is independently overridable.
                // rx2_freq=0 / rx2_dr=0xFF are sentinels meaning "not set";
                // unset fields keep the region's PHY_DEF_RX2 value.
                uint32_t rx2_freq = s_lora_obj ? s_lora_obj->rx2_freq : 0;
                uint8_t  rx2_dr   = s_lora_obj ? s_lora_obj->rx2_dr   : 0xFF;
                if (rx2_freq != 0 || rx2_dr != 0xFF) {
                    mib.Type = MIB_RX2_CHANNEL;
                    LoRaMacMibGetRequestConfirm(&mib);
                    if (rx2_freq != 0) mib.Param.Rx2Channel.Frequency = rx2_freq;
                    if (rx2_dr != 0xFF) mib.Param.Rx2Channel.Datarate = rx2_dr;
                    uint32_t eff_freq = mib.Param.Rx2Channel.Frequency;
                    uint8_t  eff_dr   = mib.Param.Rx2Channel.Datarate;
                    LoRaMacMibSetRequestConfirm(&mib);

                    mib.Type = MIB_RX2_DEFAULT_CHANNEL;
                    mib.Param.Rx2DefaultChannel.Frequency = eff_freq;
                    mib.Param.Rx2DefaultChannel.Datarate  = eff_dr;
                    LoRaMacMibSetRequestConfirm(&mib);
                    esp_rom_printf("lorawan: RX2 freq=%lu DR=%u\n",
                                   (unsigned long)eff_freq, (unsigned)eff_dr);
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

                    // Duty cycle: no dedicated MIB in v4.7.0 — read it via the
                    // NVM blob (MacGroup2.DutyCycleOn is set from the region
                    // PHY default during LoRaMacInitialization).
                    mib.Type = MIB_NVM_CTXS;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK)
                        s_lora_obj->duty_cycle_enabled = mib.Param.Contexts->MacGroup2.DutyCycleOn;
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
                // Capture the duty-cycle wait time the MAC computed for this
                // request. On LORAMAC_STATUS_OK this is also the delay until
                // the internal TxDelayedTimer fires when the MAC has queued
                // the frame instead of sending it immediately, so it is the
                // value time_until_tx() needs for callers waiting on the
                // current TX path. Always populated (the MAC initialises it
                // to 0 at the top of LoRaMacMcpsRequest).
                if (s_lora_obj) {
                    s_lora_obj->last_dc_wait_ms  = (uint32_t)mcps.ReqReturn.DutyCycleWaitTime;
                    s_lora_obj->last_dc_query_us = esp_timer_get_time();
                }
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

                    // Duty cycle is part of MacGroup2 → restored above.
                    s_lora_obj->duty_cycle_enabled =
                        s_restored_ctx.MacGroup2.DutyCycleOn;
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
                case 4: // Duty cycle on/off. LoRaMAC-node v4.7.0 has no MIB for
                        // this; the only public entry point is the test API,
                        // named "Test" because disabling duty cycle is non-
                        // conformant on public ISM bands. Exposed here for
                        // bench testing on private gateways.
                    LoRaMacTestSetDutyCycleOn(cmd.set_params.duty_cycle);
                    if (s_lora_obj) s_lora_obj->duty_cycle_enabled = cmd.set_params.duty_cycle;
                    break;
                default:
                    break;
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_REQUEST_LINK_CHECK: {
                // MLME_LINK_CHECK queues a LinkCheckReq MAC command that
                // piggy-backs on the next uplink. The network responds with
                // LinkCheckAns in that uplink's RX window; mlme_confirm then
                // fires with DemodMargin and NbGateways.
                //
                // Like request_device_time(), this call does NOT initiate an
                // uplink — the caller must follow with send() so the piggy-
                // backed request actually goes over the air.
                MlmeReq_t mlme;
                mlme.Type = MLME_LINK_CHECK;
                LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: MLME_LINK_CHECK request error %d\n", (int)st);
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_REQUEST_DEVICE_TIME: {
                // MLME_DEVICE_TIME queues a DeviceTimeReq MAC command that
                // piggy-backs on the next uplink.  The DeviceTimeAns arrives
                // in the RX1/RX2 window of that uplink and is processed in
                // LoRaMac.c (SysTimeSet + mlme_confirm fires with status OK).
                //
                // This call itself does NOT initiate an uplink — the user
                // must call send() afterwards (or wait for the next scheduled
                // send) to carry the piggy-backed request over the air.
                MlmeReq_t mlme;
                mlme.Type = MLME_DEVICE_TIME;
                LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: MLME_DEVICE_TIME request error %d\n", (int)st);
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_CLOCK_SYNC_ENABLE: {
                // Register LmhpClockSync (port 202) with the shim. Idempotent:
                // re-registering simply re-runs Init with a fresh buffer.
                if (lorawan_clock_sync_register()) {
                    if (s_lora_obj) s_lora_obj->clock_sync_enabled = true;
                    esp_rom_printf("lorawan: clock sync package enabled\n");
                } else {
                    esp_rom_printf("lorawan: clock sync register failed\n");
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_CLOCK_SYNC_REQUEST: {
                // Fires LmhpClockSyncAppTimeReq: queues an internal DeviceTimeReq
                // (via OnDeviceTimeRequest → MLME_DEVICE_TIME) and emits a port-202
                // AppTimeReq uplink immediately through the shim's LmHandlerSend.
                // Unlike request_device_time(), this already causes an uplink —
                // no follow-up send() is required.
                lorawan_clock_sync_app_time_req();
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_REQUEST_REJOIN: {
                // MLME_REJOIN_{0,1,2} sends a ReJoin-request frame over the
                // air (SendReJoinReq → ScheduleTx), unlike DeviceTimeReq /
                // LinkCheckReq which only piggy-back. No follow-up send() is
                // required — the ReJoin frame is the uplink.
                //
                // Type 0: announce presence, may trigger re-keying (1.1 only).
                // Type 1: periodic, carries JoinEUI+DevEUI, may receive Join-Accept.
                // Type 2: trigger session key refresh (1.1 only).
                MlmeReq_t mlme;
                switch (cmd.rejoin_type) {
                case 0:  mlme.Type = MLME_REJOIN_0; break;
                case 1:  mlme.Type = MLME_REJOIN_1; break;
                default: mlme.Type = MLME_REJOIN_2; break;
                }
                LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
                if (st != LORAMAC_STATUS_OK) {
                    esp_rom_printf("lorawan: MLME_REJOIN_%u request error %d\n",
                                   (unsigned)cmd.rejoin_type, (int)st);
                } else {
                    esp_rom_printf("lorawan: ReJoin type %u request sent\n",
                                   (unsigned)cmd.rejoin_type);
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_PING_SLOT_PERIODICITY: {
                // Only stored on the object; applied when CMD_SET_CLASS(CLASS_B)
                // fires MLME_PING_SLOT_INFO. Changing it after Class B is
                // already active requires re-requesting the class.
                if (s_lora_obj) {
                    s_lora_obj->ping_slot_periodicity = cmd.ping_periodicity;
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            case CMD_SET_CLASS: {
                // LoRaMAC-node v4.7.0 does not expose LoRaMacRequestClass / MLME_CLASS_C_SWITCH.
                // Class A <-> Class C is done via MIB_DEVICE_CLASS (instantaneous).
                // Class A -> Class B is multi-step: BEACON_ACQUISITION → PINGSLOT_INFO
                // → MIB_DEVICE_CLASS=CLASS_B; orchestrated via s_classb_* flags
                // and the retry block below.
                DeviceClass_t new_class = (DeviceClass_t)cmd.device_class;

                if (new_class == CLASS_B) {
                    // Prerequisite: time sync must already be established so the
                    // beacon-timing window is bounded. Require a prior
                    // request_device_time() + send() (or clock_sync_request).
                    if (!s_lora_obj || !s_lora_obj->time_synced) {
                        esp_rom_printf("lorawan: CLASS_B requires time sync "
                                       "(call request_device_time() + send() first)\n");
                        xEventGroupSetBits(s_events, EVT_CLASS_ERROR);
                        break;
                    }

                    // Current class must be CLASS_A to start acquisition.
                    mib.Type = MIB_DEVICE_CLASS;
                    LoRaMacMibGetRequestConfirm(&mib);
                    if (mib.Param.Class != CLASS_A) {
                        esp_rom_printf("lorawan: CLASS_B switch requires current class CLASS_A\n");
                        xEventGroupSetBits(s_events, EVT_CLASS_ERROR);
                        break;
                    }

                    MlmeReq_t mlme;
                    mlme.Type = MLME_BEACON_ACQUISITION;
                    LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
                    if (st != LORAMAC_STATUS_OK) {
                        esp_rom_printf("lorawan: MLME_BEACON_ACQUISITION error %d\n",
                                       (int)st);
                        xEventGroupSetBits(s_events, EVT_CLASS_ERROR);
                        break;
                    }
                    s_classb_active              = true;
                    s_classb_need_ping_slot_req  = false;
                    s_classb_need_switch_class   = false;
                    s_classb_periodicity         = s_lora_obj->ping_slot_periodicity;

                    esp_rom_printf("lorawan: Class B acquisition started (periodicity=%u)\n",
                                   (unsigned)s_classb_periodicity);
                    // Async: EVT_CLASS_OK only signals that the request was accepted.
                    // The actual MIB_DEVICE_CLASS=CLASS_B happens later and is
                    // reported via the on_class_change callback.
                    xEventGroupSetBits(s_events, EVT_CLASS_OK);
                    break;
                }

                // Class A <-> Class C path. MIB_DEVICE_CLASS set fails if
                // current class is B and the target is anything other than A.
                mib.Type = MIB_DEVICE_CLASS;
                LoRaMacMibGetRequestConfirm(&mib);
                DeviceClass_t cur_class = mib.Param.Class;

                // Leaving Class B cleanly: stop the state machine first so no
                // pending flag re-triggers PingSlotInfoReq after the switch.
                if (cur_class == CLASS_B) {
                    s_classb_active              = false;
                    s_classb_need_ping_slot_req  = false;
                    s_classb_need_switch_class   = false;
                }

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

            case CMD_MC_ADD: {
                // Local multicast setup — keys provisioned by the app out of
                // band. LoRaMacMcChannelSetup copies the key bytes into the
                // soft-SE immediately, so the stack-local buffers we pass in
                // don't need to outlive this call.
                cmd_mc_add_t *a = &cmd.mc_add;
                uint8_t nwk_key[16], app_key[16];
                memcpy(nwk_key, a->mc_nwk_s_key, 16);
                memcpy(app_key, a->mc_app_s_key, 16);

                McChannelParams_t ch = {
                    .IsRemotelySetup = false,
                    .IsEnabled       = true,
                    .GroupID         = (AddressIdentifier_t)a->group,
                    .Address         = a->addr,
                    .FCountMin       = a->f_count_min,
                    .FCountMax       = a->f_count_max,
                };
                ch.McKeys.Session.McAppSKey = app_key;
                ch.McKeys.Session.McNwkSKey = nwk_key;
                // RxParams is configured separately via CMD_MC_RX_PARAMS. The
                // MAC only validates RxParams inside LoRaMacMcChannelSetupRxParams,
                // so zero-init here is fine.

                LoRaMacStatus_t st = LoRaMacMcChannelSetup(&ch);
                if (st == LORAMAC_STATUS_OK && s_lora_obj) {
                    lorawan_mc_group_t *g = &s_lora_obj->mc_groups[a->group];
                    g->active        = true;
                    g->is_remote     = false;
                    g->rx_params_set = false;
                    g->addr          = a->addr;
                    g->f_count_min   = a->f_count_min;
                    g->f_count_max   = a->f_count_max;
                    esp_rom_printf("lorawan: multicast group %u added addr=0x%08lx\n",
                                   (unsigned)a->group, (unsigned long)a->addr);
                    xEventGroupSetBits(s_events, EVT_COMPLETED);
                } else {
                    esp_rom_printf("lorawan: LoRaMacMcChannelSetup failed: %d\n", (int)st);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR); // reuse generic error bit
                }
                break;
            }

            case CMD_MC_RX_PARAMS: {
                cmd_mc_rx_params_t *p = &cmd.mc_rx_params;
                McRxParams_t rx;
                rx.Class = (DeviceClass_t)p->device_class;
                if (rx.Class == CLASS_B) {
                    rx.Params.ClassB.Frequency   = p->frequency;
                    rx.Params.ClassB.Datarate    = p->datarate;
                    rx.Params.ClassB.Periodicity = p->periodicity;
                } else {
                    rx.Params.ClassC.Frequency = p->frequency;
                    rx.Params.ClassC.Datarate  = p->datarate;
                }
                uint8_t status = 0;
                LoRaMacStatus_t st = LoRaMacMcChannelSetupRxParams(
                    (AddressIdentifier_t)p->group, &rx, &status);
                if (st == LORAMAC_STATUS_OK && s_lora_obj) {
                    lorawan_mc_group_t *g = &s_lora_obj->mc_groups[p->group];
                    g->rx_params_set  = true;
                    g->rx_class       = rx.Class;
                    g->rx_frequency   = p->frequency;
                    g->rx_datarate    = p->datarate;
                    g->rx_periodicity = (rx.Class == CLASS_B) ? p->periodicity : 0;
                    esp_rom_printf("lorawan: multicast group %u rx params %c freq=%lu DR=%d\n",
                                   (unsigned)p->group,
                                   rx.Class == CLASS_B ? 'B' : 'C',
                                   (unsigned long)p->frequency, (int)p->datarate);
                    xEventGroupSetBits(s_events, EVT_COMPLETED);
                } else {
                    esp_rom_printf("lorawan: LoRaMacMcChannelSetupRxParams failed: %d status=0x%02x\n",
                                   (int)st, (unsigned)status);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                }
                break;
            }

            case CMD_MC_REMOVE: {
                uint8_t group = cmd.mc_group;
                LoRaMacStatus_t st = LoRaMacMcChannelDelete((AddressIdentifier_t)group);
                if (st == LORAMAC_STATUS_OK && s_lora_obj) {
                    memset(&s_lora_obj->mc_groups[group], 0,
                           sizeof(lorawan_mc_group_t));
                    esp_rom_printf("lorawan: multicast group %u removed\n",
                                   (unsigned)group);
                    xEventGroupSetBits(s_events, EVT_COMPLETED);
                } else {
                    esp_rom_printf("lorawan: LoRaMacMcChannelDelete failed: %d\n", (int)st);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                }
                break;
            }

            case CMD_REMOTE_MCAST_ENABLE: {
                bool ok = lorawan_remote_mcast_setup_register();
                if (s_lora_obj) s_lora_obj->remote_mcast_enabled = ok;
                xEventGroupSetBits(s_events, ok ? EVT_COMPLETED : EVT_TX_ERROR);
                break;
            }

            case CMD_DEINIT: {
                // Ordered teardown from task context so MAC / radio state is
                // only ever touched from here.  If the MAC is in the middle
                // of a TX or RX window LoRaMacDeInitialization returns BUSY;
                // we still force the radio to sleep and proceed with the
                // HAL-level teardown below — the task is about to exit so
                // any lingering MAC state becomes irrelevant.
                if (s_mac_initialized) {
                    LoRaMacStatus_t ds = LoRaMacDeInitialization();
                    if (ds != LORAMAC_STATUS_OK) {
                        // MAC was mid-TX/RX; force-quiesce the radio before
                        // the SPI bus goes away.
                        lorawan_radio_sleep();
                    }
                }

                // Drop LmHandler package registrations and release the
                // shim-owned fragmentation buffer.
                lorawan_packages_deinit();

                // Deregister radio DIO ISRs and free the SPI bus.
                bool is_sx1276_local = s_lora_obj ? s_lora_obj->is_sx1276 : true;
                if (is_sx1276_local) {
                    SX1276IoDeInit();
                } else {
                    SX126xIoDeInit();
                    sx126x_spi_mutex_deinit();
                }

                // Stop+delete the shared esp_timer so no stray LoRaMAC
                // timer callback can fire after the task is gone.
                lorawan_timer_deinit();

                BoardDeInitMcu();

                s_mac_initialized       = false;
                s_otaa_active           = false;
                s_otaa_retry_needed     = false;
                s_nvram_autosave_needed = false;
                s_classb_active             = false;
                s_classb_need_ping_slot_req = false;
                s_classb_need_switch_class  = false;
                s_classb_need_device_time_req = false;

                // Signal the Python side; it will drop the queues and event
                // group after we are gone.
                xEventGroupSetBits(s_events, EVT_COMPLETED);

                // Self-delete. FreeRTOS' idle task reclaims the TCB/stack.
                // The for-loop below is unreachable but guards against any
                // port where vTaskDelete returns control briefly before the
                // scheduler tears the task down.
                vTaskDelete(NULL);
                for (;;) {
                    vTaskDelay(portMAX_DELAY);
                }
            }

            case CMD_FRAGMENTATION_ENABLE: {
                uint32_t sz = cmd.frag_enable.buffer_size;
                // Allocate via malloc (raw heap) — the MAC accesses this from
                // the LoRaWAN task, which is outside MicroPython's GC scope.
                // Leak-safe in practice: fragmentation_enable() is normally
                // called once per boot. A second call with a larger buffer
                // would leak the previous buffer, so reject that here.
                if (s_lora_obj && s_lora_obj->fragmentation_enabled) {
                    esp_rom_printf("lorawan: fragmentation already enabled\n");
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                    break;
                }
                uint8_t *buf = (uint8_t *)malloc(sz);
                if (buf == NULL) {
                    esp_rom_printf("lorawan: fragmentation buffer alloc failed (%lu)\n",
                                   (unsigned long)sz);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                    break;
                }
                memset(buf, 0, sz);
                bool ok = lorawan_fragmentation_register(buf, sz);
                if (ok && s_lora_obj) {
                    s_lora_obj->fragmentation_enabled     = true;
                    s_lora_obj->fragmentation_buffer_size = sz;
                    xEventGroupSetBits(s_events, EVT_COMPLETED);
                } else {
                    free(buf);
                    xEventGroupSetBits(s_events, EVT_TX_ERROR);
                }
                break;
            }

            case CMD_QUERY: {
                // Read-only MIB / TX-info queries dispatched from Python.
                // The Python caller is blocked on EVT_COMPLETED while we run,
                // so the s_query_* statics are safe to write here and read
                // there without further synchronisation.
                switch (cmd.query_type) {
                case LW_QUERY_DEV_ADDR: {
                    s_query_dev_addr = 0;
                    mib.Type = MIB_DEV_ADDR;
                    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
                        s_query_dev_addr = mib.Param.DevAddr;
                    }
                    break;
                }
                case LW_QUERY_MAX_PAYLOAD_LEN: {
                    s_query_max_payload_len = 0;
                    LoRaMacTxInfo_t tx_info;
                    // size=0 query: txInfo.MaxPossibleApplicationDataSize is
                    // populated whenever LORAMAC_STATUS_OK or LENGTH_ERROR is
                    // returned (only PARAMETER_INVALID / MAC_COMMAD_ERROR skip
                    // it). Treat the populated path as success.
                    LoRaMacStatus_t st = LoRaMacQueryTxPossible(0, &tx_info);
                    if (st == LORAMAC_STATUS_OK || st == LORAMAC_STATUS_LENGTH_ERROR) {
                        s_query_max_payload_len = tx_info.MaxPossibleApplicationDataSize;
                    }
                    break;
                }
                default:
                    break;
                }
                xEventGroupSetBits(s_events, EVT_COMPLETED);
                break;
            }

            default:
                break;
            }
        }

        // Class B orchestration — MLME callbacks only flipped flags; we re-issue
        // the follow-up MLME/MCPS/MIB requests from task context here.
        if (s_classb_need_ping_slot_req && s_classb_active) {
            s_classb_need_ping_slot_req = false;

            MlmeReq_t mlme;
            mlme.Type = MLME_PING_SLOT_INFO;
            mlme.Req.PingSlotInfo.PingSlot.Fields.Periodicity = s_classb_periodicity;
            mlme.Req.PingSlotInfo.PingSlot.Fields.RFU = 0;
            LoRaMacStatus_t st = LoRaMacMlmeRequest(&mlme);
            if (st == LORAMAC_STATUS_OK) {
                // Flush the MAC command with an empty unconfirmed uplink so the
                // piggy-backed PingSlotInfoReq actually reaches the server
                // (mirrors LmHandlerPingSlotReq in LmHandler.c).
                McpsReq_t req;
                req.Type = MCPS_UNCONFIRMED;
                req.Req.Unconfirmed.fPort       = 0;
                req.Req.Unconfirmed.fBuffer     = NULL;
                req.Req.Unconfirmed.fBufferSize = 0;
                req.Req.Unconfirmed.Datarate    =
                    s_lora_obj ? s_lora_obj->channels_datarate : DR_0;
                LoRaMacMcpsRequest(&req);
            } else {
                esp_rom_printf("lorawan: MLME_PING_SLOT_INFO error %d\n", (int)st);
                if (st != LORAMAC_STATUS_BUSY &&
                    st != LORAMAC_STATUS_DUTYCYCLE_RESTRICTED) {
                    // Transient vs permanent: retry only on BUSY/duty-cycle.
                    s_classb_active = false;
                }
            }
        }

        if (s_classb_need_switch_class && s_classb_active) {
            s_classb_need_switch_class = false;

            MibRequestConfirm_t mib2;
            mib2.Type = MIB_DEVICE_CLASS;
            mib2.Param.Class = CLASS_B;
            LoRaMacStatus_t st = LoRaMacMibSetRequestConfirm(&mib2);
            if (st == LORAMAC_STATUS_OK) {
                if (s_lora_obj) {
                    s_lora_obj->device_class = CLASS_B;
                    if (s_lora_obj->class_callback != mp_const_none) {
                        mp_sched_schedule(MP_OBJ_FROM_PTR(&lorawan_class_trampoline_obj),
                                          MP_OBJ_NEW_SMALL_INT((mp_int_t)CLASS_B));
                    }
                }
                esp_rom_printf("lorawan: device class -> B\n");
            } else {
                // BeaconMode==1 && Assigned==1 should hold here; if the MIB set
                // rejects it, log and drop out of the Class B state machine.
                esp_rom_printf("lorawan: MIB_DEVICE_CLASS=B failed: %d\n", (int)st);
                s_classb_active = false;
            }
        }

        if (s_classb_need_device_time_req && s_classb_active) {
            // Beacon acquisition failed — request a fresh DeviceTimeAns so the
            // next MLME_BEACON_ACQUISITION has a bounded search window, then
            // re-issue the beacon acquisition itself.
            s_classb_need_device_time_req = false;

            MlmeReq_t dt;
            dt.Type = MLME_DEVICE_TIME;
            LoRaMacMlmeRequest(&dt);

            MlmeReq_t ba;
            ba.Type = MLME_BEACON_ACQUISITION;
            LoRaMacMlmeRequest(&ba);
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

        // Let every registered LmHandler package drain any deferred work
        // (Remote Multicast Setup session-start class switches, Fragmentation
        // BlockAckDelay replies). Safe to call unconditionally — no-op when
        // no packages are registered.
        lorawan_packages_process();
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
        // RX2 window — each field independently falls through to the region's
        // PHY_DEF_RX2 (standard LoRaWAN DR_0 / region default freq). For TTN
        // EU868, typically just rx2_dr=DR_3 is needed (the 869.525 MHz freq
        // matches the EU868 default).
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

    // Finaliser-backed allocation so the GC's sweep during soft-reset
    // (gc_sweep_all in ports/esp32/main.c) fires __del__ → deinit()
    // automatically. Without this, Ctrl-D leaves the FreeRTOS task alive
    // and dereferencing a freed lorawan_obj_t on the next radio IRQ.
    lorawan_obj_t *self = mp_obj_malloc_with_finaliser(lorawan_obj_t, type);
    self->initialized      = false;
    self->joined           = false;
    self->is_sx1276        = true;
    self->region           = (LoRaMacRegion_t)args[ARG_region].u_int;
    self->device_class     = (DeviceClass_t)args[ARG_device_class].u_int;
    self->lorawan_version  = (uint8_t)args[ARG_lorawan_version].u_int;

    // RX2 override: each kwarg is independent. Unset fields fall through to
    // the MAC's PHY_DEF_RX2 (standard LoRaWAN DR_0 / region default freq).
    // TTN EU868 users typically only need rx2_dr=DR_3 — the 869.525 MHz freq
    // is already the EU868 region default. Sentinel: rx2_dr=0xFF, rx2_freq=0
    // mean "not overridden".
    int rx2_dr_arg   = args[ARG_rx2_dr].u_int;
    int rx2_freq_arg = args[ARG_rx2_freq].u_int;
    self->rx2_dr   = (rx2_dr_arg   < 0) ? 0xFF : (uint8_t)rx2_dr_arg;
    self->rx2_freq = (rx2_freq_arg < 0) ? 0    : (uint32_t)rx2_freq_arg;
    self->rx_callback        = mp_const_none;
    self->tx_callback        = mp_const_none;
    self->class_callback     = mp_const_none;
    self->time_sync_callback = mp_const_none;
    self->tx_counter       = 0;
    self->tx_time_on_air   = 0;
    self->last_tx_ack      = false;
    self->rx_counter       = 0;
    self->rssi             = 0;
    self->snr              = 0;
    self->channels_datarate = DR_0;
    self->adr_enabled       = false;
    // Duty cycle: cached value gets overwritten in CMD_INIT from the region
    // PHY default (true on EU868 / EU433). last_dc_* stay zero until the
    // first send() actually goes through the MAC.
    self->duty_cycle_enabled = true;
    self->last_dc_wait_ms    = 0;
    self->last_dc_query_us   = 0;
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

    self->time_synced      = false;
    self->network_time_gps = 0;
    self->link_check_received = false;
    self->link_check_margin   = 0;
    self->link_check_gw_count = 0;
    self->clock_sync_enabled  = false;

    self->beacon_callback       = mp_const_none;
    self->ping_slot_periodicity = 0;
    self->beacon_info_valid     = false;
    self->beacon_last_state     = 0;
    self->beacon_time_seconds   = 0;
    self->beacon_freq           = 0;
    self->beacon_datarate       = 0;
    self->beacon_rssi           = 0;
    self->beacon_snr            = 0;
    self->beacon_gw_info_desc   = 0;
    memset(self->beacon_gw_info, 0, sizeof(self->beacon_gw_info));

    memset(self->mc_groups, 0, sizeof(self->mc_groups));
    self->remote_mcast_enabled  = false;
    self->fragmentation_enabled = false;
    self->fragmentation_buffer_size = 0;
    self->fragmentation_last_size   = 0;
    self->fragmentation_last_status = 0;
    self->fragmentation_progress_callback = mp_const_none;
    self->fragmentation_done_callback     = mp_const_none;

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

    enum { ARG_data, ARG_port, ARG_confirmed, ARG_datarate, ARG_timeout };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_data,      MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_port,      MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int  = 1} },
        { MP_QSTR_confirmed, MP_ARG_KW_ONLY  | MP_ARG_BOOL, {.u_bool = false} },
        // DR_0=SF12 .. DR_5=SF7; default DR_0 for maximum range on first uplinks
        { MP_QSTR_datarate,  MP_ARG_KW_ONLY  | MP_ARG_INT,  {.u_int  = DR_0} },
        // timeout in seconds. Default 120 s covers a duty-cycled DR_0 uplink
        // (the MAC may queue the frame internally via TxDelayedTimer when the
        // band is restricted). Pass None to block until TX completes.
        { MP_QSTR_timeout,   MP_ARG_KW_ONLY  | MP_ARG_OBJ,  {.u_obj  = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Resolve timeout: None → portMAX_DELAY (wait forever); int seconds → ticks.
    // Default (kwarg omitted) → 120 s. A non-positive int is treated as None
    // for forward compatibility with code that does `timeout=0` to mean "wait".
    TickType_t wait_ticks;
    mp_obj_t timeout_obj = args[ARG_timeout].u_obj;
    if (timeout_obj == MP_OBJ_NULL) {
        wait_ticks = pdMS_TO_TICKS(120000);
    } else if (timeout_obj == mp_const_none) {
        wait_ticks = portMAX_DELAY;
    } else {
        int timeout_s = mp_obj_get_int(timeout_obj);
        wait_ticks = (timeout_s > 0)
                     ? pdMS_TO_TICKS((uint32_t)timeout_s * 1000)
                     : portMAX_DELAY;
    }

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

    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           EVT_TX_DONE | EVT_TX_ERROR,
                                           pdTRUE, pdFALSE,
                                           wait_ticks);

    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("send failed"));
    }
    if (!(bits & EVT_TX_DONE)) {
        // The MAC may still have the frame queued (TxDelayedTimer pending);
        // it will transmit later on its own. tx_counter advances when it does.
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
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_tx_dr),
                      mp_obj_new_int(self->last_tx_dr));
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_tx_freq),
                      mp_obj_new_int_from_uint(self->last_tx_freq));
    // Convert MAC index → dBm so this is consistent with tx_power(); zero
    // before the first TX maps to region max, which is acceptable since
    // tx_counter==0 already tells the caller no uplink has happened yet.
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_last_tx_power),
                      mp_obj_new_int(tx_power_to_dbm(self->last_tx_power)));
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

// duty_cycle(enabled=None) — getter/setter for the regional duty-cycle gate.
// On by default for compliance (EU868 / EU433 enforce a 1 % air-time per band).
// Disabling is non-conformant on public ISM bands and intended for bench
// testing on a private gateway only.
static mp_obj_t lorawan_duty_cycle(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (n_args == 1) {
        return mp_obj_new_bool(self->duty_cycle_enabled);
    }
    bool enable = mp_obj_is_true(args[1]);
    if (!enable) {
        mp_printf(&mp_plat_print,
                  "lorawan: duty cycle disabled — non-conformant on public ISM bands\n");
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_SET_PARAMS };
    cmd.set_params.type       = 4;
    cmd.set_params.duty_cycle = enable;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_duty_cycle_obj, 1, 2, lorawan_duty_cycle);

// time_until_tx() — milliseconds until the next uplink is allowed.
// Reflects the DutyCycleWaitTime captured from the most recent McpsRequest:
// when the MAC accepts a frame but defers it via TxDelayedTimer, this is the
// remaining time until that timer fires. Returns 0 when no send has run yet,
// when duty cycle is off, or once the wait window has elapsed.
static mp_obj_t lorawan_time_until_tx(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (self->last_dc_wait_ms == 0 || self->last_dc_query_us == 0) {
        return mp_obj_new_int(0);
    }
    int64_t elapsed_ms = (esp_timer_get_time() - self->last_dc_query_us) / 1000;
    if (elapsed_ms < 0) elapsed_ms = 0;
    int64_t remaining = (int64_t)self->last_dc_wait_ms - elapsed_ms;
    if (remaining < 0) remaining = 0;
    return mp_obj_new_int((mp_int_t)remaining);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_time_until_tx_obj, lorawan_time_until_tx);

// request_class(cls) — request a switch to CLASS_A, CLASS_B or CLASS_C.
// CLASS_A / CLASS_C: LoRaMAC-node performs the transition synchronously via
// MIB_DEVICE_CLASS; on_class_change fires before the call returns.
// CLASS_B: requires time sync (call request_device_time() + send() first) and
// is multi-step — the call returns once beacon acquisition starts; the actual
// class change is signalled later via on_class_change(CLASS_B). Beacon events
// (lock, loss, acquisition fail) are surfaced via on_beacon(cb).
static mp_obj_t lorawan_request_class(mp_obj_t self_in, mp_obj_t cls_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    int cls = mp_obj_get_int(cls_in);
    if (cls != CLASS_A && cls != CLASS_B && cls != CLASS_C) {
        mp_raise_ValueError(MP_ERROR_TEXT("class must be CLASS_A, CLASS_B or CLASS_C"));
    }
    if (cls == CLASS_B && !self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("CLASS_B requires a joined session"));
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

// region() — returns the LORAMAC_REGION_* the object was constructed with.
// Cached on lorawan_obj_t.region at __init__ — no MAC round-trip.
static mp_obj_t lorawan_region(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int((int)self->region);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_region_obj, lorawan_region);

// dev_addr() — returns the current 32-bit DevAddr from MIB_DEV_ADDR.
// After OTAA this is the server-assigned value; after ABP it is whatever the
// caller passed to join_abp(). Returns 0 before any activation.
static mp_obj_t lorawan_dev_addr(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_QUERY };
    cmd.query_type = LW_QUERY_DEV_ADDR;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_obj_new_int_from_uint(s_query_dev_addr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_dev_addr_obj, lorawan_dev_addr);

// max_payload_len() — returns the maximum app payload size (bytes) for the
// next uplink at the current DR, accounting for any MAC commands the stack
// has queued (e.g. LinkCheckReq, DeviceTimeReq). Wraps LoRaMacQueryTxPossible
// with size=0 so the call does not actually submit a frame. Lets callers
// slice payloads before send() instead of catching a late LENGTH_ERROR.
static mp_obj_t lorawan_max_payload_len(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_QUERY };
    cmd.query_type = LW_QUERY_MAX_PAYLOAD_LEN;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_obj_new_int(s_query_max_payload_len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_max_payload_len_obj, lorawan_max_payload_len);

// request_device_time() — queue an MLME_DEVICE_TIME request.
// Sends a DeviceTimeReq MAC command piggy-backed on the next uplink; the
// network responds with DeviceTimeAns in the same RX window.  The LoRaMAC
// stack processes the answer (SysTimeSet) and fires mlme_confirm with
// MLME_DEVICE_TIME, which we capture into self.time_synced / self.network_time_gps.
//
// This function is non-blocking with respect to the actual time answer —
// it only waits for the MAC to accept the MLME request.  The caller must
// trigger an uplink (send()) afterwards so the DeviceTimeReq actually goes
// over the air.
static mp_obj_t lorawan_request_device_time(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (!self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not joined"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_REQUEST_DEVICE_TIME };
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_request_device_time_obj, lorawan_request_device_time);

// network_time() — returns GPS epoch seconds from the last DeviceTimeAns,
// or None if no sync has happened yet. The stored value is the epoch at the
// moment the answer was processed; callers that need "now" should combine
// this with a local tick offset. For most applications the granularity
// (~second) is sufficient and reading again after a fresh sync is simpler.
static mp_obj_t lorawan_network_time(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->time_synced) {
        return mp_const_none;
    }
    return mp_obj_new_int_from_uint(self->network_time_gps);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_network_time_obj, lorawan_network_time);

// link_check() — queue an MLME_LINK_CHECK request and return the most recent
// LinkCheckAns as {"margin": N, "gw_count": N}, or None if no answer has been
// received yet. Mirrors the request_device_time() / network_time() pair, but
// folded into one call since the result is tiny and has no dedicated event.
//
// The MLME queues a LinkCheckReq MAC command that piggy-backs on the next
// uplink; the caller must trigger send() afterwards for the request to go
// over the air. On the following cycle (after the RX window closes and
// mlme_confirm fires), link_check() returns the fresh margin and gw_count.
static mp_obj_t lorawan_link_check(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (!self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not joined"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_REQUEST_LINK_CHECK };
    send_cmd_wait(&cmd, EVT_COMPLETED);

    if (!self->link_check_received) {
        return mp_const_none;
    }
    mp_obj_t d = mp_obj_new_dict(2);
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_margin),
                      mp_obj_new_int(self->link_check_margin));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_gw_count),
                      mp_obj_new_int(self->link_check_gw_count));
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_link_check_obj, lorawan_link_check);

// rejoin(type=0) — send a LoRaWAN 1.1 ReJoin-request frame.
// type=0: announce presence; network may re-derive session keys (1.1 only).
// type=1: periodic rejoin, carries DevEUI+JoinEUI; a Join-Accept may follow.
// type=2: request session key refresh (1.1 only).
//
// ReJoin only makes sense after a successful OTAA join. Unlike link_check()
// and request_device_time() (which piggy-back on the next uplink), a rejoin
// is itself an uplink frame — no follow-up send() is required. The call
// returns as soon as the MAC has accepted the request; the optional
// Join-Accept arrives later and is processed by the MAC internally.
static mp_obj_t lorawan_rejoin(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (!self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not joined"));
    }
    int type = (n_args > 1) ? mp_obj_get_int(args[1]) : 0;
    if (type < 0 || type > 2) {
        mp_raise_ValueError(MP_ERROR_TEXT("rejoin type must be 0, 1 or 2"));
    }
    lorawan_cmd_data_t cmd;
    cmd.cmd = CMD_REQUEST_REJOIN;
    cmd.rejoin_type = (uint8_t)type;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_rejoin_obj, 1, 2, lorawan_rejoin);

// clock_sync_enable() — register the LmhpClockSync application-layer package
// (port 202, LoRa-Alliance Clock Sync v1.0.0). The package can only be
// registered after the MAC stack is initialised; registering before join() is
// fine, but clock_sync_request() itself requires a joined session to transmit.
static mp_obj_t lorawan_clock_sync_enable(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_CLOCK_SYNC_ENABLE };
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_obj_new_bool(self->clock_sync_enabled);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_clock_sync_enable_obj, lorawan_clock_sync_enable);

// clock_sync_request() — trigger LmhpClockSyncAppTimeReq: emits an AppTimeReq
// uplink on port 202 AND piggy-backs a MAC-layer DeviceTimeReq. Unlike
// request_device_time() (which only queues a piggy-back MAC command), this
// already causes an uplink to go out — no follow-up send() is required.
//
// The server's AppTimeAns or MAC-layer DeviceTimeAns arrives in the RX window
// of that uplink; LmhpClockSync updates SysTime and invokes the shim's
// OnSysTimeUpdate hook, which refreshes self.network_time_gps and fires the
// registered on_time_sync callback.
static mp_obj_t lorawan_clock_sync_request(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (!self->joined) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not joined"));
    }
    if (!self->clock_sync_enabled) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("clock sync not enabled; call clock_sync_enable() first"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_CLOCK_SYNC_REQUEST };
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_clock_sync_request_obj, lorawan_clock_sync_request);

// synced_time() — return the corrected time as GPS epoch seconds (live, based
// on SysTimeGet), or None if no sync has happened yet. Unlike network_time()
// which returns the snapshot captured at the moment of the last sync, this
// advances with the local clock between syncs.
static mp_obj_t lorawan_synced_time(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->time_synced) {
        return mp_const_none;
    }
    SysTime_t now = SysTimeGet();
    uint32_t gps_epoch = (now.Seconds >= UNIX_GPS_EPOCH_OFFSET)
                         ? (now.Seconds - UNIX_GPS_EPOCH_OFFSET) : 0;
    return mp_obj_new_int_from_uint(gps_epoch);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_synced_time_obj, lorawan_synced_time);

// on_time_sync(callback) — register callback(gps_epoch_seconds) for every
// successful DeviceTimeAns. Pass None to deregister.
static mp_obj_t lorawan_on_time_sync(mp_obj_t self_in, mp_obj_t cb) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_time_sync: callback must be callable or None"));
    }
    self->time_sync_callback = cb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_on_time_sync_obj, lorawan_on_time_sync);

// Build a Python dict from the cached BeaconInfo snapshot on the object.
// Must run in VM context (allocates dict/bytes); called from the trampoline
// and from beacon_state().  beacon_time is GPS-epoch seconds per the
// LoRaWAN spec (BeaconInfo_t.Time is already GPS epoch).
static mp_obj_t lorawan_beacon_info_dict(lorawan_obj_t *self) {
    mp_obj_t d = mp_obj_new_dict(0);
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_state),
                      mp_obj_new_int(self->beacon_last_state));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_time),
                      mp_obj_new_int_from_uint(self->beacon_time_seconds));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_freq),
                      mp_obj_new_int_from_uint(self->beacon_freq));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_datarate),
                      mp_obj_new_int(self->beacon_datarate));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_rssi),
                      mp_obj_new_int(self->beacon_rssi));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_snr),
                      mp_obj_new_int(self->beacon_snr));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_gw_info_desc),
                      mp_obj_new_int(self->beacon_gw_info_desc));
    mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_gw_info),
                      mp_obj_new_bytes(self->beacon_gw_info, 6));
    return d;
}

// on_beacon(callback) — register callback(state, info) for beacon events.
// state is one of lorawan.BEACON_* constants. info is a dict (see beacon_state)
// or None if no beacon has been received yet (e.g. BEACON_ACQUISITION_FAIL).
// Pass None to deregister.
static mp_obj_t lorawan_on_beacon(mp_obj_t self_in, mp_obj_t cb) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (cb != mp_const_none && !mp_obj_is_callable(cb)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_beacon: callback must be callable or None"));
    }
    self->beacon_callback = cb;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_on_beacon_obj, lorawan_on_beacon);

// beacon_state() — return the last known beacon info dict or None if no
// beacon has been received yet. Keys: state, time (GPS epoch s), freq (Hz),
// datarate, rssi, snr, gw_info_desc, gw_info (6 bytes).
static mp_obj_t lorawan_beacon_state(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->beacon_info_valid) {
        return mp_const_none;
    }
    return lorawan_beacon_info_dict(self);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_beacon_state_obj, lorawan_beacon_state);

// ping_slot_periodicity(N=None) — getter/setter for the Class B ping-slot
// periodicity (0..7). Period between ping slots is 2^N seconds. Only takes
// effect on the next request_class(CLASS_B) — changing it while already in
// Class B requires a class-B renegotiation.
static mp_obj_t lorawan_ping_slot_periodicity(size_t n_args, const mp_obj_t *args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    if (n_args == 1) {
        return mp_obj_new_int(self->ping_slot_periodicity);
    }
    int n = mp_obj_get_int(args[1]);
    if (n < 0 || n > 7) {
        mp_raise_ValueError(MP_ERROR_TEXT("periodicity must be 0..7"));
    }
    lorawan_cmd_data_t cmd;
    cmd.cmd = CMD_PING_SLOT_PERIODICITY;
    cmd.ping_periodicity = (uint8_t)n;
    send_cmd_wait(&cmd, EVT_COMPLETED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_ping_slot_periodicity_obj, 1, 2,
                                            lorawan_ping_slot_periodicity);

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

// multicast_add(group, addr, mc_nwk_s_key, mc_app_s_key, f_count_min=0, f_count_max=0xFFFFFFFF)
// — local multicast setup (keys provisioned out-of-band). After add(),
// configure RX params with multicast_rx_params() and switch class to C (or B).
static mp_obj_t lorawan_multicast_add(size_t n_args, const mp_obj_t *pos_args,
                                       mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    enum { ARG_group, ARG_addr, ARG_nwk_key, ARG_app_key,
           ARG_fcnt_min, ARG_fcnt_max };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_group,         MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_addr,          MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_mc_nwk_s_key,  MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_mc_app_s_key,  MP_ARG_REQUIRED | MP_ARG_OBJ },
        { MP_QSTR_f_count_min,   MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
        // 0xFFFFFFFF doesn't fit in int32 so we default via INT_MAX and let
        // the user pass a Python int for the full range.
        { MP_QSTR_f_count_max,   MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed), allowed, args);

    int group = args[ARG_group].u_int;
    if (group < 0 || group > 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("group must be 0..3"));
    }
    mp_buffer_info_t nwk_buf, app_buf;
    mp_get_buffer_raise(args[ARG_nwk_key].u_obj, &nwk_buf, MP_BUFFER_READ);
    mp_get_buffer_raise(args[ARG_app_key].u_obj, &app_buf, MP_BUFFER_READ);
    if (nwk_buf.len != 16 || app_buf.len != 16) {
        mp_raise_ValueError(MP_ERROR_TEXT("multicast keys must be 16 bytes"));
    }

    uint32_t fcnt_max = 0xFFFFFFFFu;
    if (args[ARG_fcnt_max].u_obj != mp_const_none) {
        fcnt_max = (uint32_t)mp_obj_get_int_truncated(args[ARG_fcnt_max].u_obj);
    }

    lorawan_cmd_data_t cmd = { .cmd = CMD_MC_ADD };
    cmd.mc_add.group       = (uint8_t)group;
    cmd.mc_add.addr        = (uint32_t)mp_obj_get_int_truncated(args[ARG_addr].u_obj);
    cmd.mc_add.f_count_min = (uint32_t)args[ARG_fcnt_min].u_int;
    cmd.mc_add.f_count_max = fcnt_max;
    memcpy(cmd.mc_add.mc_nwk_s_key, nwk_buf.buf, 16);
    memcpy(cmd.mc_add.mc_app_s_key, app_buf.buf, 16);

    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_COMPLETED | EVT_TX_ERROR);
    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("multicast_add failed"));
    }
    return mp_obj_new_int(group);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_multicast_add_obj, 1, lorawan_multicast_add);

// multicast_rx_params(group, device_class, frequency, datarate, periodicity=None)
// — configure the multicast RX window. Class C: only frequency+datarate.
// Class B: also periodicity (0..7 → 2^N seconds between ping slots).
static mp_obj_t lorawan_multicast_rx_params(size_t n_args, const mp_obj_t *pos_args,
                                             mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    enum { ARG_group, ARG_class, ARG_freq, ARG_dr, ARG_periodicity };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_group,        MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_device_class, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_frequency,    MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_datarate,     MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_periodicity,  MP_ARG_KW_ONLY  | MP_ARG_INT, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed), allowed, args);

    int group = args[ARG_group].u_int;
    int cls   = args[ARG_class].u_int;
    if (group < 0 || group > 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("group must be 0..3"));
    }
    if (cls != CLASS_B && cls != CLASS_C) {
        mp_raise_ValueError(
            MP_ERROR_TEXT("device_class must be CLASS_B or CLASS_C for multicast"));
    }

    lorawan_cmd_data_t cmd = { .cmd = CMD_MC_RX_PARAMS };
    cmd.mc_rx_params.group        = (uint8_t)group;
    cmd.mc_rx_params.device_class = (uint8_t)cls;
    cmd.mc_rx_params.frequency    = (uint32_t)args[ARG_freq].u_int;
    cmd.mc_rx_params.datarate     = (int8_t)args[ARG_dr].u_int;
    cmd.mc_rx_params.periodicity  = (uint16_t)args[ARG_periodicity].u_int;

    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_COMPLETED | EVT_TX_ERROR);
    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("multicast_rx_params failed"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_multicast_rx_params_obj, 1,
                                   lorawan_multicast_rx_params);

// multicast_remove(group) — detach a group and clear its crypto state.
static mp_obj_t lorawan_multicast_remove(mp_obj_t self_in, mp_obj_t group_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    int group = mp_obj_get_int(group_in);
    if (group < 0 || group > 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("group must be 0..3"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_MC_REMOVE };
    cmd.mc_group = (uint8_t)group;
    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_COMPLETED | EVT_TX_ERROR);
    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("multicast_remove failed"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(lorawan_multicast_remove_obj, lorawan_multicast_remove);

// multicast_list() — returns list of active group dicts.
static mp_obj_t lorawan_multicast_list(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < 4; i++) {
        lorawan_mc_group_t *g = &self->mc_groups[i];
        if (!g->active) continue;
        mp_obj_t d = mp_obj_new_dict(0);
        mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_group),
                          mp_obj_new_int(i));
        mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_addr),
                          mp_obj_new_int_from_uint(g->addr));
        mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_is_remote),
                          mp_obj_new_bool(g->is_remote));
        mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_f_count_min),
                          mp_obj_new_int_from_uint(g->f_count_min));
        mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_f_count_max),
                          mp_obj_new_int_from_uint(g->f_count_max));
        if (g->rx_params_set) {
            mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_device_class),
                              mp_obj_new_int((int)g->rx_class));
            mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_frequency),
                              mp_obj_new_int_from_uint(g->rx_frequency));
            mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_datarate),
                              mp_obj_new_int(g->rx_datarate));
            if (g->rx_class == CLASS_B) {
                mp_obj_dict_store(d, MP_ROM_QSTR(MP_QSTR_periodicity),
                                  mp_obj_new_int(g->rx_periodicity));
            }
        }
        mp_obj_list_append(list, d);
    }
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_multicast_list_obj, lorawan_multicast_list);

// remote_multicast_enable() — register LmhpRemoteMcastSetup (port 200).
// After registration the network server may provision groups remotely via
// McGroupSetupReq / McGroupClassCSessionReq; the package forwards the class
// switches to the MAC automatically (via LmHandlerRequestClass in the shim).
static mp_obj_t lorawan_remote_multicast_enable(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    lorawan_cmd_data_t cmd = { .cmd = CMD_REMOTE_MCAST_ENABLE };
    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_COMPLETED | EVT_TX_ERROR);
    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("remote_multicast_enable failed"));
    }
    return mp_obj_new_bool(self->remote_mcast_enabled);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_remote_multicast_enable_obj,
                                  lorawan_remote_multicast_enable);

// fragmentation_enable(buffer_size, on_progress=None, on_done=None) — register
// LmhpFragmentation (port 201) for FUOTA-base support. Allocates a flat RAM
// buffer that the FragDecoder writes fragments into; callbacks fire in
// MicroPython context via mp_sched_schedule.
//   on_progress(counter, nb, size, lost)
//   on_done(status, size)       — status 0 = success; status > 0 = fragments lost
static mp_obj_t lorawan_fragmentation_enable(size_t n_args, const mp_obj_t *pos_args,
                                              mp_map_t *kw_args) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (!self->initialized) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("not initialized"));
    }
    enum { ARG_buffer_size, ARG_on_progress, ARG_on_done };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_buffer_size, MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_on_progress, MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_on_done,     MP_ARG_KW_ONLY  | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
                     MP_ARRAY_SIZE(allowed), allowed, args);

    int sz = args[ARG_buffer_size].u_int;
    if (sz <= 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("buffer_size must be > 0"));
    }
    if (args[ARG_on_progress].u_obj != mp_const_none &&
        !mp_obj_is_callable(args[ARG_on_progress].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_progress must be callable or None"));
    }
    if (args[ARG_on_done].u_obj != mp_const_none &&
        !mp_obj_is_callable(args[ARG_on_done].u_obj)) {
        mp_raise_TypeError(MP_ERROR_TEXT("on_done must be callable or None"));
    }

    self->fragmentation_progress_callback = args[ARG_on_progress].u_obj;
    self->fragmentation_done_callback     = args[ARG_on_done].u_obj;

    lorawan_cmd_data_t cmd = { .cmd = CMD_FRAGMENTATION_ENABLE };
    cmd.frag_enable.buffer_size = (uint32_t)sz;
    EventBits_t bits = send_cmd_wait_result(&cmd, EVT_COMPLETED | EVT_TX_ERROR);
    if (bits & EVT_TX_ERROR) {
        mp_raise_msg(&mp_type_RuntimeError,
                     MP_ERROR_TEXT("fragmentation_enable failed"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lorawan_fragmentation_enable_obj, 1,
                                   lorawan_fragmentation_enable);

// fragmentation_data() — return the full reassembled buffer (bytes) after
// on_done has fired, or None if fragmentation was never enabled. Caller is
// expected to slice the buffer down to the size reported by on_done.
static mp_obj_t lorawan_fragmentation_data(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->fragmentation_enabled) {
        return mp_const_none;
    }
    uint32_t sz = 0;
    const uint8_t *buf = lorawan_fragmentation_buffer(&sz);
    if (buf == NULL || sz == 0) {
        return mp_const_none;
    }
    return mp_obj_new_bytes(buf, (size_t)sz);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_fragmentation_data_obj,
                                  lorawan_fragmentation_data);

// deinit() — ordered teardown so a fresh lorawan.LoRaWAN(...) can be
// created in the same REPL session. Idempotent: calling after the task has
// already been torn down is a no-op. Also wired as __del__ on the type so
// the GC sweep that runs during soft-reset (Ctrl-D in REPL) fires it
// automatically, matching the pattern used by machine.I2S, I2CTarget etc.
static mp_obj_t lorawan_deinit(mp_obj_t self_in) {
    lorawan_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Already torn down — nothing to do.
    if (s_task_handle == NULL) {
        if (self) self->initialized = false;
        return mp_const_none;
    }

    // Dispatch CMD_DEINIT; the task handles the MAC + HAL teardown and
    // self-deletes via vTaskDelete(NULL). We wait bounded — even if the
    // MAC was stuck on a TX/RX the task still reaches vTaskDelete.
    lorawan_cmd_data_t cmd = { .cmd = CMD_DEINIT };
    xEventGroupClearBits(s_events, EVT_COMPLETED);
    xQueueSend(s_cmd_queue, &cmd, portMAX_DELAY);
    xTaskNotifyGive(s_task_handle);
    xEventGroupWaitBits(s_events, EVT_COMPLETED,
                        pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));

    // Give FreeRTOS' idle task one tick to actually reclaim the TCB
    // before we delete the primitives the (now-dead) task was using.
    vTaskDelay(pdMS_TO_TICKS(10));

    if (s_cmd_queue) { vQueueDelete(s_cmd_queue); s_cmd_queue = NULL; }
    if (s_rx_queue)  { vQueueDelete(s_rx_queue);  s_rx_queue  = NULL; }
    if (s_events)    { vEventGroupDelete(s_events); s_events = NULL; }

    s_task_handle = NULL;
    s_lora_obj    = NULL;

    // Clear the hardware TX-power override too — a fresh LoRaWAN() should
    // start with MAC-driven TX power unless the user re-requests override.
    g_tx_power_hw_override = LORAWAN_TX_POWER_NO_OVERRIDE;

    if (self) self->initialized = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_deinit_obj, lorawan_deinit);

// `with lorawan.LoRaWAN(...) as lw:` support — __exit__ guarantees deinit()
// on scope exit even if the block raised.
static mp_obj_t lorawan_enter(mp_obj_t self_in) {
    return self_in;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lorawan_enter_obj, lorawan_enter);

static mp_obj_t lorawan_exit(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    return lorawan_deinit(args[0]);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lorawan_exit_obj, 1, 4, lorawan_exit);

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
    { MP_ROM_QSTR(MP_QSTR_duty_cycle),     MP_ROM_PTR(&lorawan_duty_cycle_obj) },
    { MP_ROM_QSTR(MP_QSTR_time_until_tx),  MP_ROM_PTR(&lorawan_time_until_tx_obj) },
    { MP_ROM_QSTR(MP_QSTR_request_class),  MP_ROM_PTR(&lorawan_request_class_obj) },
    { MP_ROM_QSTR(MP_QSTR_device_class),   MP_ROM_PTR(&lorawan_device_class_obj) },
    { MP_ROM_QSTR(MP_QSTR_region),         MP_ROM_PTR(&lorawan_region_obj) },
    { MP_ROM_QSTR(MP_QSTR_dev_addr),       MP_ROM_PTR(&lorawan_dev_addr_obj) },
    { MP_ROM_QSTR(MP_QSTR_max_payload_len), MP_ROM_PTR(&lorawan_max_payload_len_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_class_change), MP_ROM_PTR(&lorawan_on_class_change_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_beacon),      MP_ROM_PTR(&lorawan_on_beacon_obj) },
    { MP_ROM_QSTR(MP_QSTR_beacon_state),   MP_ROM_PTR(&lorawan_beacon_state_obj) },
    { MP_ROM_QSTR(MP_QSTR_ping_slot_periodicity),
                                            MP_ROM_PTR(&lorawan_ping_slot_periodicity_obj) },
    { MP_ROM_QSTR(MP_QSTR_request_device_time), MP_ROM_PTR(&lorawan_request_device_time_obj) },
    { MP_ROM_QSTR(MP_QSTR_network_time),   MP_ROM_PTR(&lorawan_network_time_obj) },
    { MP_ROM_QSTR(MP_QSTR_link_check),     MP_ROM_PTR(&lorawan_link_check_obj) },
    { MP_ROM_QSTR(MP_QSTR_rejoin),         MP_ROM_PTR(&lorawan_rejoin_obj) },
    { MP_ROM_QSTR(MP_QSTR_on_time_sync),   MP_ROM_PTR(&lorawan_on_time_sync_obj) },
    { MP_ROM_QSTR(MP_QSTR_clock_sync_enable),  MP_ROM_PTR(&lorawan_clock_sync_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_clock_sync_request), MP_ROM_PTR(&lorawan_clock_sync_request_obj) },
    { MP_ROM_QSTR(MP_QSTR_synced_time),        MP_ROM_PTR(&lorawan_synced_time_obj) },
    // Multicast (Session 14)
    { MP_ROM_QSTR(MP_QSTR_multicast_add),       MP_ROM_PTR(&lorawan_multicast_add_obj) },
    { MP_ROM_QSTR(MP_QSTR_multicast_rx_params), MP_ROM_PTR(&lorawan_multicast_rx_params_obj) },
    { MP_ROM_QSTR(MP_QSTR_multicast_remove),    MP_ROM_PTR(&lorawan_multicast_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_multicast_list),      MP_ROM_PTR(&lorawan_multicast_list_obj) },
    { MP_ROM_QSTR(MP_QSTR_remote_multicast_enable),
                                                 MP_ROM_PTR(&lorawan_remote_multicast_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_fragmentation_enable), MP_ROM_PTR(&lorawan_fragmentation_enable_obj) },
    { MP_ROM_QSTR(MP_QSTR_fragmentation_data),   MP_ROM_PTR(&lorawan_fragmentation_data_obj) },
    // Lifecycle (Session 16). __del__ fires on GC sweep, including the
    // sweep done by the ESP32 port during soft-reset (Ctrl-D in REPL).
    { MP_ROM_QSTR(MP_QSTR_deinit),               MP_ROM_PTR(&lorawan_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),              MP_ROM_PTR(&lorawan_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__),            MP_ROM_PTR(&lorawan_enter_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),             MP_ROM_PTR(&lorawan_exit_obj) },
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
    return mp_obj_new_str("0.15.0", 6);
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
    // Class B beacon state codes (passed to on_beacon callback first arg)
    { MP_ROM_QSTR(MP_QSTR_BEACON_ACQUISITION_OK),   MP_ROM_INT(LW_BEACON_ACQUISITION_OK) },
    { MP_ROM_QSTR(MP_QSTR_BEACON_ACQUISITION_FAIL), MP_ROM_INT(LW_BEACON_ACQUISITION_FAIL) },
    { MP_ROM_QSTR(MP_QSTR_BEACON_LOCKED),           MP_ROM_INT(LW_BEACON_LOCKED) },
    { MP_ROM_QSTR(MP_QSTR_BEACON_NOT_FOUND),        MP_ROM_INT(LW_BEACON_NOT_FOUND) },
    { MP_ROM_QSTR(MP_QSTR_BEACON_LOST),             MP_ROM_INT(LW_BEACON_LOST) },
};
static MP_DEFINE_CONST_DICT(lorawan_module_globals, lorawan_module_globals_table);

const mp_obj_module_t lorawan_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&lorawan_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lorawan, lorawan_module);
