// lmhandler_shim.c — minimal LmHandler adapter (see lmhandler_shim.h)
//
// LmhpClockSync.c is written against the LmHandler API. We provide only the
// three functions the package actually calls, plus a tiny package registry
// that routes MCPS confirm / indication events back to the package.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "esp_rom_sys.h"

#include "LoRaMac.h"
#include "LmHandler.h"
#include "LmhPackage.h"
#include "LmhpClockSync.h"
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
