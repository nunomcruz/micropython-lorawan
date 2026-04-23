"""
time_sync.py — synchronise with network time via DeviceTimeReq and Clock Sync.

Two independent paths update the same internal time snapshot:

  1. MAC DeviceTimeReq (LoRaWAN 1.0.3+). Piggy-backs on the next uplink; the
     answer arrives in that uplink's RX window. Cheap — no dedicated frame.
  2. Clock Sync application package (LoRa-Alliance v1.0.0, port 202). Sends
     its own AppTimeReq (which also carries a DeviceTimeReq as a MAC option).

Both update .network_time_gps / .time_synced and both fire the on_time_sync
callback. Use DeviceTimeReq when you just need a one-shot timestamp; use the
Clock Sync package if you want periodic re-sync with drift compensation.
"""

import tbeam
import lorawan
from time import sleep

DEV_EUI  = bytes.fromhex("70B3D57ED0000000")
JOIN_EUI = bytes.fromhex("0000000000000000")
APP_KEY  = bytes.fromhex("00000000000000000000000000000000")

# Offset between GPS epoch (1980-01-06) and Unix epoch (1970-01-01). The API
# returns GPS epoch seconds; add this to convert to Unix time.
GPS_TO_UNIX_OFFSET = 315964800


def format_gps(gps_seconds):
    """Return a best-effort ISO-ish string from GPS epoch seconds."""
    if gps_seconds is None:
        return "<not synced>"
    try:
        import time
        t = time.gmtime(gps_seconds + GPS_TO_UNIX_OFFSET)
        return "{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}Z".format(*t[:6])
    except Exception:
        return f"gps={gps_seconds}"


def on_time_sync(gps_seconds):
    print(f"[time_sync] updated to {format_gps(gps_seconds)} (gps={gps_seconds})")


def main():
    hw = tbeam.detect()
    print("tbeam:", hw)

    lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_3)

    # Install the callback before any request so the first sync is reported.
    lw.on_time_sync(on_time_sync)

    # Restore session if we have one saved; otherwise join OTAA.
    try:
        lw.nvram_restore()
    except OSError:
        pass

    if not lw.joined():
        print("joining OTAA...")
        lw.join_otaa(dev_eui=DEV_EUI, join_eui=JOIN_EUI,
                     app_key=APP_KEY, timeout=60)
        print("join OK")

    # ----- Path 1: MAC DeviceTimeReq -----
    print("\n--- DeviceTimeReq (MAC command, piggy-backs on next uplink) ---")
    lw.request_device_time()
    # request_device_time() only queues the MAC command. An uplink is still
    # needed to carry it over the air; any uplink works.
    lw.send(b"")
    print(f"network_time (snapshot): {format_gps(lw.network_time())}")
    print(f"synced_time (live):      {format_gps(lw.synced_time())}")

    lw.nvram_save()

    # ----- Path 2: Clock Sync application package (port 202) -----
    print("\n--- Clock Sync package (AppTimeReq on port 202) ---")
    try:
        ok = lw.clock_sync_enable()
        print(f"clock_sync_enable → {ok}")
    except RuntimeError as e:
        print("clock_sync_enable failed:", e)
        return

    try:
        # Force DR_0 (SF12/125) for the AppTimeReq. Without this, ADR may have
        # ratcheted the DR up to DR_5 (SF7), and range-limited port-202 frames
        # silently fail to reach the gateway (mlme_confirm fires with
        # MLME_DEVICE_TIME status=4 and no sync callback). The DR override is
        # scoped to this single AppTimeReq — LmhpClockSync restores the previous
        # MIB_CHANNELS_DATARATE on the MCPS confirm, so subsequent telemetry
        # uplinks keep the ADR-negotiated DR.
        lw.clock_sync_request(datarate=lorawan.DR_0)
    except RuntimeError as e:
        print("clock_sync_request failed:", e)
        return

    # Give the MAC a moment to process the answer.
    sleep(2)
    print(f"network_time (snapshot): {format_gps(lw.network_time())}")
    print(f"synced_time (live):      {format_gps(lw.synced_time())}")

    # ----- Bonus: synchronous link_check at DR_0 -----
    # Default link_check() only queues the MAC command and relies on the next
    # uplink to carry it. Use send_now=True to also emit an empty port-1 frame
    # and block until the RX window closes; costs one extra uplink (FCntUp +1)
    # but returns the fresh margin/gw_count in a single call. Pair with
    # datarate=DR_0 for range-critical probes.
    print("\n--- link_check (synchronous, DR_0) ---")
    try:
        info = lw.link_check(send_now=True, datarate=lorawan.DR_0)
    except RuntimeError as e:
        print("link_check failed:", e)
        info = None
    if info is None:
        print("link_check: no answer (network did not send LinkCheckAns)")
    else:
        print(f"link margin: {info['margin']} dB, seen by {info['gw_count']} gateway(s)")

    lw.nvram_save()


if __name__ == "__main__":
    main()
