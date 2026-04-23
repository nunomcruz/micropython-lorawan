// lmhandler_shim.h — minimal LmHandler adapter for hosting LmHandler packages
//
// The project uses LoRaMAC-node directly (not the full LmHandler layer), but
// the Clock Sync package (LmhpClockSync.c) is written against the LmHandler
// API. This shim provides the tiny subset required by the package:
//
//   - LmHandlerIsBusy()          — query MAC readiness before sending
//   - LmHandlerSend()            — emit an uplink on an application port
//   - LmHandlerPackageRegister() — store the package and wire its callbacks
//
// It also exposes dispatch hooks that modlorawan.c's existing MAC callbacks
// call to forward MCPS confirm / indication events into the registered
// packages. No LoRaMacInitialization() conflict: the shim never installs its
// own primitives.

#ifndef LORAWAN_LMHANDLER_SHIM_H
#define LORAWAN_LMHANDLER_SHIM_H

#include <stdint.h>
#include <stdbool.h>
#include "LoRaMac.h"

#ifdef __cplusplus
extern "C" {
#endif

// Forward MCPS events from the existing MAC callbacks into any registered
// packages (e.g. LmhpClockSync).  Safe to call with NULL pointers.
void lorawan_packages_on_mcps_confirm(McpsConfirm_t *c);
void lorawan_packages_on_mcps_indication(McpsIndication_t *ind);

// Called by the shim's OnSysTimeUpdate hook whenever the Clock Sync package
// has updated SysTime after an AppTimeAns.  Implemented in modlorawan.c so
// it can snapshot the corrected time onto the active lorawan_obj_t.
void lorawan_on_sys_time_update(bool is_synchronized, int32_t correction);

// Register LmhpClockSync (port 202) with the shim.  Idempotent.
// Returns true on success.  Must be called from the LoRaWAN task context.
bool lorawan_clock_sync_register(void);

// Trigger LmhpClockSyncAppTimeReq() — sends an AppTimeReq on port 202 and
// piggy-backs a DeviceTimeReq MAC command.  Must be called from the LoRaWAN
// task context so the MAC is only touched from a single thread.
// Returns true if the request was queued.
bool lorawan_clock_sync_app_time_req(void);

#ifdef __cplusplus
}
#endif

#endif // LORAWAN_LMHANDLER_SHIM_H
