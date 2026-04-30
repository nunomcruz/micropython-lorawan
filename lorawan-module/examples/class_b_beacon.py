"""
class_b_beacon.py — join, acquire Class B beacon, receive on ping slots.

Class B switches the device from Class A (RX only after TX) to a scheduled
mode where the gateway broadcasts a beacon every 128 s. The device locks
onto the beacon and opens a small RX window at a deterministic offset,
repeated every 2^N seconds ("ping slots"). This lets the network push a
downlink at any time without first asking the device to transmit.

Prerequisites:
  * The device must be time-synced (via MAC DeviceTimeReq) before Class B
    can be requested.
  * The gateway you're talking to must support Class B (many community
    gateways don't forward the beacon — check with the LNS operator).

Test flow:
  1. Join OTAA.
  2. Sync time via DeviceTimeReq (piggy-backed on an empty uplink).
  3. Set ping_slot_periodicity (PingSlotPeriod = 2^N seconds).
  4. Request Class B — beacon acquisition runs asynchronously. Watch the
     on_beacon callback for state transitions.
  5. Once locked, queue a downlink from the LNS console and wait for it
     to arrive on a ping slot (on_recv fires with the frame).
"""

import tbeam
import lorawan
from time import sleep, ticks_ms, ticks_diff

DEV_EUI  = bytes.fromhex("70B3D57ED0000000")
JOIN_EUI = bytes.fromhex("0000000000000000")
APP_KEY  = bytes.fromhex("00000000000000000000000000000000")

# Ping-slot period is 2^N seconds. N=4 → 16 s between ping slots.
# Smaller N = more frequent slots = higher power. Max is 7 (128 s).
PING_SLOT_PERIODICITY = 4

# Max wall-clock we'll wait for the beacon to lock before giving up.
BEACON_WAIT_SECONDS = 200


_beacon_state = [lorawan.BEACON_NOT_FOUND]


def beacon_name(state):
    for label in ("BEACON_ACQUISITION_OK", "BEACON_ACQUISITION_FAIL",
                  "BEACON_LOCKED", "BEACON_NOT_FOUND", "BEACON_LOST"):
        if getattr(lorawan, label) == state:
            return label
    return f"?({state})"


def on_beacon(state, info):
    _beacon_state[0] = state
    print(f"[beacon] {beacon_name(state)}")
    if info:
        print(f"         time={info['time']} freq={info['freq']} "
              f"dr={info['datarate']} rssi={info['rssi']} snr={info['snr']}")


def on_class_change(new_class):
    names = {lorawan.CLASS_A: "A", lorawan.CLASS_B: "B", lorawan.CLASS_C: "C"}
    print(f"[class] now CLASS_{names.get(new_class, '?')}")


def on_downlink(data, port, rssi, snr, mc):
    kind = "mcast" if mc else "unicast"
    print(f"[rx] {kind} port={port} rssi={rssi} snr={snr}: {data!r}")


def main():
    hw = tbeam.detect()
    print("tbeam:", hw)

    lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)
    lw.on_beacon(on_beacon)
    lw.on_class_change(on_class_change)
    lw.on_recv(on_downlink)

    try:
        lw.nvram_restore()
    except OSError:
        pass

    if not lw.joined():
        print("joining OTAA...")
        lw.join_otaa(dev_eui=DEV_EUI, join_eui=JOIN_EUI,
                     app_key=APP_KEY, timeout=60)

    # --- time sync (required for Class B) ---
    print("\nsyncing time...")
    lw.request_device_time()
    lw.send(b"")  # carries the DeviceTimeReq

    # synced_time() returns the live GPS-epoch second once any sync path has
    # answered. Pre-1.1 the equivalent snapshot was network_time(); that name
    # is going away in v1.1 (see TODO Session 22).
    t = lw.synced_time()
    if t is None:
        print("no time sync — the network did not answer DeviceTimeReq.")
        print("Class B cannot be requested without a time sync. Aborting.")
        return
    print(f"time sync OK (gps={t})")

    # --- configure ping-slot periodicity before the class switch ---
    lw.ping_slot_periodicity(PING_SLOT_PERIODICITY)
    print(f"ping slots every {1 << PING_SLOT_PERIODICITY} s")

    # --- request Class B (asynchronous; beacon acquisition drives state) ---
    print("\nrequesting Class B (beacon acquisition starts now)...")
    try:
        lw.device_class(lorawan.CLASS_B)
    except OSError as e:
        print("device_class failed:", e)
        return

    # Wait for beacon lock. on_beacon updates _beacon_state from MLME context.
    start = ticks_ms()
    while ticks_diff(ticks_ms(), start) < BEACON_WAIT_SECONDS * 1000:
        if _beacon_state[0] == lorawan.BEACON_LOCKED:
            break
        if _beacon_state[0] == lorawan.BEACON_ACQUISITION_FAIL:
            print("beacon acquisition failed — network likely lacks Class B")
            return
        sleep(2)
    else:
        print(f"no beacon within {BEACON_WAIT_SECONDS} s — aborting")
        return

    print("\nbeacon locked. Waiting for downlinks on ping slots.")
    print("Queue one from the LNS console (Messaging → Downlink → port 10).")
    print("Ctrl-C to stop.\n")

    try:
        while True:
            sleep(10)
            # Periodic heartbeat — refreshes FCntUp, which TTN relies on
            # to keep the device "alive" for downlink routing.
            try:
                lw.send(b"")
                lw.nvram_save()
            except (OSError, RuntimeError) as e:
                print("heartbeat send failed:", e)
    except KeyboardInterrupt:
        print("stopped")
        lw.nvram_save()


if __name__ == "__main__":
    main()
