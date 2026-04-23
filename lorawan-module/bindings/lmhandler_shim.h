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

// Register LmhpRemoteMcastSetup (port 200).  After registration the server
// can configure multicast groups remotely via McGroupSetupReq.
bool lorawan_remote_mcast_setup_register(void);

// Fragmentation (LmhpFragmentation, port 201) callbacks bridged out to
// modlorawan.c so Python can receive progress / completion events in VM
// context via mp_sched_schedule.  Called by the C package from task context.
void lorawan_on_fragmentation_progress(uint16_t frag_counter,
                                       uint16_t frag_nb,
                                       uint8_t  frag_size,
                                       uint16_t frag_nb_lost);
void lorawan_on_fragmentation_done(int32_t status, uint32_t size);

// Fragmentation read/write — backed by a flat RAM buffer owned by the
// caller (allocated when fragmentation_enable() is called).  Exposed here
// so the FragDecoder can write received fragments into application memory.
bool lorawan_fragmentation_register(uint8_t *buffer, uint32_t buffer_size);

// Read back a range from the fragmentation buffer (used by Python to
// retrieve the reassembled payload after on_done fires).  Returns NULL
// if fragmentation is not currently enabled.
const uint8_t *lorawan_fragmentation_buffer(uint32_t *out_size);

// Drain any pending Process() events from every registered package.
// Must be called from the LoRaWAN task main loop.
void lorawan_packages_process(void);

// Clear the package registry and release any shim-owned buffers (currently
// the fragmentation RAM buffer allocated via fragmentation_enable()). Called
// from modlorawan.c's deinit path so the next lorawan.LoRaWAN(...) starts
// from a clean slate.
void lorawan_packages_deinit(void);

// LmHandler shim for Class change — used by LmhpRemoteMcastSetup to flip
// to Class C at session start and back to Class A at session end.  This
// runs from package Process() which the task invokes, so it is safe to
// touch the MAC directly (MIB_DEVICE_CLASS).
struct sDeviceClass; // forward decl, real type in LoRaMac.h / LoRaMacTypes.h

#ifdef __cplusplus
}
#endif

#endif // LORAWAN_LMHANDLER_SHIM_H
