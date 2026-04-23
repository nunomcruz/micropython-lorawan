// lmhandler_shim.c — minimal LmHandler adapter (see lmhandler_shim.h)
//
// LmhpClockSync.c is written against the LmHandler API. We provide only the
// three functions the package actually calls, plus a tiny package registry
// that routes MCPS confirm / indication events back to the package.

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_rom_sys.h"

#include "LoRaMac.h"
#include "LmHandler.h"
#include "LmhPackage.h"
#include "LmhpClockSync.h"
#include "LmhpRemoteMcastSetup.h"
#include "LmhpFragmentation.h"
#include "FragDecoder.h"
#include "systime.h"

#include "lmhandler_shim.h"

// Per LmhPackage.h the max number of simultaneously-registered packages.
#define SHIM_PKG_MAX            PKG_MAX_NUMBER

// Scratch buffer handed to the package for composing MAC-layer answers
// (e.g. the response to an APP_TIME_PERIOD_REQ).  222 B is the largest
// application payload at DR5 EU868.
#define SHIM_PKG_BUFFER_SIZE    222

static LmhPackage_t *s_packages[SHIM_PKG_MAX];
static uint8_t       s_pkg_buffer[SHIM_PKG_BUFFER_SIZE];

// ---- Callbacks injected into registered packages ----

// Called by LmhpClockSync when it wants the MAC stack to piggy-back a
// DeviceTimeReq on the next uplink.  Separate from the AppTimeReq itself.
static LmHandlerErrorStatus_t shim_on_device_time_request(void) {
    MlmeReq_t mlme;
    mlme.Type = MLME_DEVICE_TIME;
    return (LoRaMacMlmeRequest(&mlme) == LORAMAC_STATUS_OK)
           ? LORAMAC_HANDLER_SUCCESS : LORAMAC_HANDLER_ERROR;
}

// Called by LmhpClockSync after it applies a time correction from the
// server's AppTimeAns.  Propagate to modlorawan.c so the Python-visible
// time_synced / network_time_gps snapshot stays fresh.
static void shim_on_sys_time_update(bool is_synchronized, int32_t correction) {
    lorawan_on_sys_time_update(is_synchronized, correction);
}

// ---- LmHandler subset required by LmhpClockSync.c ----

bool LmHandlerIsBusy(void) {
    // The package uses this to abort AppTimeReq when the MAC cannot
    // currently send.  LoRaMacQueryTxPossible with a zero-length payload
    // returns OK when an uplink could be scheduled immediately.
    LoRaMacTxInfo_t info;
    return LoRaMacQueryTxPossible(0, &info) != LORAMAC_STATUS_OK;
}

LmHandlerErrorStatus_t LmHandlerSend(LmHandlerAppData_t *appData,
                                     LmHandlerMsgTypes_t isTxConfirmed) {
    if (appData == NULL) {
        return LORAMAC_HANDLER_ERROR;
    }

    // Use the current MAC data rate so the package-level uplink obeys ADR.
    int8_t datarate = DR_0;
    MibRequestConfirm_t mib;
    mib.Type = MIB_CHANNELS_DATARATE;
    if (LoRaMacMibGetRequestConfirm(&mib) == LORAMAC_STATUS_OK) {
        datarate = mib.Param.ChannelsDatarate;
    }

    McpsReq_t mcps;
    if (isTxConfirmed == LORAMAC_HANDLER_CONFIRMED_MSG) {
        mcps.Type = MCPS_CONFIRMED;
        mcps.Req.Confirmed.fPort       = appData->Port;
        mcps.Req.Confirmed.fBuffer     = appData->Buffer;
        mcps.Req.Confirmed.fBufferSize = appData->BufferSize;
        mcps.Req.Confirmed.Datarate    = datarate;
    } else {
        mcps.Type = MCPS_UNCONFIRMED;
        mcps.Req.Unconfirmed.fPort       = appData->Port;
        mcps.Req.Unconfirmed.fBuffer     = appData->Buffer;
        mcps.Req.Unconfirmed.fBufferSize = appData->BufferSize;
        mcps.Req.Unconfirmed.Datarate    = datarate;
    }

    return (LoRaMacMcpsRequest(&mcps) == LORAMAC_STATUS_OK)
           ? LORAMAC_HANDLER_SUCCESS : LORAMAC_HANDLER_ERROR;
}

LmHandlerErrorStatus_t LmHandlerPackageRegister(uint8_t id, void *params) {
    LmhPackage_t *pkg = NULL;
    switch (id) {
    case PACKAGE_ID_CLOCK_SYNC:
        pkg = LmphClockSyncPackageFactory();
        break;
    case PACKAGE_ID_REMOTE_MCAST_SETUP:
        pkg = LmhpRemoteMcastSetupPackageFactory();
        break;
    case PACKAGE_ID_FRAGMENTATION:
        pkg = LmhpFragmentationPackageFactory();
        break;
    default:
        return LORAMAC_HANDLER_ERROR;
    }
    if (pkg == NULL || id >= SHIM_PKG_MAX) {
        return LORAMAC_HANDLER_ERROR;
    }

    // Wire the LmHandler-provided callbacks.  OnMacMcpsRequest /
    // OnMacMlmeRequest / OnJoinRequest are unused by LmhpClockSync so we
    // leave them NULL.
    pkg->OnMacMcpsRequest    = NULL;
    pkg->OnMacMlmeRequest    = NULL;
    pkg->OnJoinRequest       = NULL;
    pkg->OnDeviceTimeRequest = shim_on_device_time_request;
#if( LMH_SYS_TIME_UPDATE_NEW_API == 1 )
    pkg->OnSysTimeUpdate     = shim_on_sys_time_update;
#else
    pkg->OnSysTimeUpdate     = NULL;
#endif

    if (pkg->Init != NULL) {
        pkg->Init(params, s_pkg_buffer, sizeof(s_pkg_buffer));
    }

    s_packages[id] = pkg;
    return LORAMAC_HANDLER_SUCCESS;
}

bool LmHandlerPackageIsInitialized(uint8_t id) {
    if (id >= SHIM_PKG_MAX || s_packages[id] == NULL) {
        return false;
    }
    return s_packages[id]->IsInitialized != NULL
           && s_packages[id]->IsInitialized();
}

// ---- Event fan-out invoked from modlorawan.c's MAC callbacks ----

void lorawan_packages_on_mcps_confirm(McpsConfirm_t *c) {
    if (c == NULL) return;
    for (uint8_t i = 0; i < SHIM_PKG_MAX; i++) {
        LmhPackage_t *pkg = s_packages[i];
        if (pkg != NULL && pkg->OnMcpsConfirmProcess != NULL) {
            pkg->OnMcpsConfirmProcess(c);
        }
    }
}

void lorawan_packages_on_mcps_indication(McpsIndication_t *ind) {
    if (ind == NULL) return;
    for (uint8_t i = 0; i < SHIM_PKG_MAX; i++) {
        LmhPackage_t *pkg = s_packages[i];
        if (pkg != NULL && pkg->OnMcpsIndicationProcess != NULL) {
            pkg->OnMcpsIndicationProcess(ind);
        }
    }
}

// ---- Thin wrappers for modlorawan.c ----

bool lorawan_clock_sync_register(void) {
    return LmHandlerPackageRegister(PACKAGE_ID_CLOCK_SYNC, NULL)
           == LORAMAC_HANDLER_SUCCESS;
}

bool lorawan_clock_sync_app_time_req(void) {
    if (!LmHandlerPackageIsInitialized(PACKAGE_ID_CLOCK_SYNC)) {
        esp_rom_printf("lorawan: clock sync package not registered\n");
        return false;
    }
    LmHandlerErrorStatus_t st = LmhpClockSyncAppTimeReq();
    if (st != LORAMAC_HANDLER_SUCCESS) {
        esp_rom_printf("lorawan: LmhpClockSyncAppTimeReq failed\n");
        return false;
    }
    return true;
}

// ---- Remote Multicast Setup ----
//
// LmhpRemoteMcastSetup.c calls LmHandlerRequestClass() when a session starts
// or stops to toggle the device class. We honour it from the package Process()
// which runs in LoRaWAN task context, so touching MIB_DEVICE_CLASS directly
// is safe (no callback context, single-threaded MAC).
LmHandlerErrorStatus_t LmHandlerRequestClass(DeviceClass_t newClass) {
    MibRequestConfirm_t mib;
    mib.Type = MIB_DEVICE_CLASS;
    mib.Param.Class = newClass;
    LoRaMacStatus_t st = LoRaMacMibSetRequestConfirm(&mib);
    return (st == LORAMAC_STATUS_OK)
           ? LORAMAC_HANDLER_SUCCESS : LORAMAC_HANDLER_ERROR;
}

bool lorawan_remote_mcast_setup_register(void) {
    return LmHandlerPackageRegister(PACKAGE_ID_REMOTE_MCAST_SETUP, NULL)
           == LORAMAC_HANDLER_SUCCESS;
}

// ---- Fragmentation (FUOTA base) ----
//
// The fragmentation package stores received fragments via FragDecoder write
// callbacks. We back the decoder with a flat RAM buffer: writes land in the
// buffer, reads serve from it. After on_done fires the reassembled payload
// is available via lorawan_fragmentation_buffer().

static uint8_t *s_frag_buffer;
static uint32_t s_frag_buffer_size;

static int8_t frag_decoder_write(uint32_t addr, uint8_t *data, uint32_t size) {
    if (s_frag_buffer == NULL || addr + size > s_frag_buffer_size) {
        return -1;
    }
    memcpy(s_frag_buffer + addr, data, size);
    return 0;
}

static int8_t frag_decoder_read(uint32_t addr, uint8_t *data, uint32_t size) {
    if (s_frag_buffer == NULL || addr + size > s_frag_buffer_size) {
        return -1;
    }
    memcpy(data, s_frag_buffer + addr, size);
    return 0;
}

static LmhpFragmentationParams_t s_frag_params = {
    .DecoderCallbacks = {
        .FragDecoderWrite = frag_decoder_write,
        .FragDecoderRead  = frag_decoder_read,
    },
    .OnProgress = NULL,
    .OnDone     = NULL,
};

static void frag_on_progress(uint16_t frag_counter, uint16_t frag_nb,
                              uint8_t frag_size, uint16_t frag_nb_lost) {
    lorawan_on_fragmentation_progress(frag_counter, frag_nb,
                                       frag_size, frag_nb_lost);
}

static void frag_on_done(int32_t status, uint32_t size) {
    lorawan_on_fragmentation_done(status, size);
}

bool lorawan_fragmentation_register(uint8_t *buffer, uint32_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return false;
    }
    s_frag_buffer      = buffer;
    s_frag_buffer_size = buffer_size;

    s_frag_params.OnProgress = frag_on_progress;
    s_frag_params.OnDone     = frag_on_done;

    return LmHandlerPackageRegister(PACKAGE_ID_FRAGMENTATION, &s_frag_params)
           == LORAMAC_HANDLER_SUCCESS;
}

const uint8_t *lorawan_fragmentation_buffer(uint32_t *out_size) {
    if (out_size) *out_size = s_frag_buffer_size;
    return s_frag_buffer;
}

// ---- Package Process() fan-out ----
//
// Packages (Remote Mcast Setup, Fragmentation) use Process() to perform
// delayed work queued from their OnMcpsIndication handlers — e.g. switching
// class at session start, answering after a BlockAckDelay. Called from the
// LoRaWAN task loop each iteration.
void lorawan_packages_process(void) {
    for (uint8_t i = 0; i < SHIM_PKG_MAX; i++) {
        LmhPackage_t *pkg = s_packages[i];
        if (pkg != NULL && pkg->Process != NULL) {
            pkg->Process();
        }
    }
}

// Drop every registered package and release buffers the shim owns so a later
// lorawan.LoRaWAN(...) can re-register packages cleanly. The fragmentation
// RAM buffer is malloc'd by modlorawan.c's CMD_FRAGMENTATION_ENABLE and
// handed over to this layer via lorawan_fragmentation_register(); we free it
// here to centralise ownership.
void lorawan_packages_deinit(void) {
    for (uint8_t i = 0; i < SHIM_PKG_MAX; i++) {
        s_packages[i] = NULL;
    }
    if (s_frag_buffer) {
        free(s_frag_buffer);
        s_frag_buffer = NULL;
    }
    s_frag_buffer_size        = 0;
    s_frag_params.OnProgress  = NULL;
    s_frag_params.OnDone      = NULL;
    memset(s_pkg_buffer, 0, sizeof(s_pkg_buffer));
}
