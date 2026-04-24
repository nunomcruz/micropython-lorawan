"""
basic_otaa.py — minimal OTAA join + uplink.

Works out of the box on any T-Beam variant (SX1276 or SX1262) using tbeam.detect()
to print the detected hardware. Edit DEV_EUI / JOIN_EUI / APP_KEY below with the
values from your LNS (TTN, ChirpStack, ...).

Persistence:
  * nvram_restore() is called on every boot so DevNonce resumes across resets.
    Skipping it causes TTN to reject the next join with "devnonce is too small".
  * nvram_save() is called after the uplink to persist the new FCntUp.
"""

import tbeam
import lorawan
from time import sleep

# ------------------------------------------------------------------
# Credentials — replace with values from your LNS console.
# All three are big-endian hex strings (the format TTN displays).
# ------------------------------------------------------------------
DEV_EUI  = bytes.fromhex("70B3D57ED0000000")
JOIN_EUI = bytes.fromhex("0000000000000000")
APP_KEY  = bytes.fromhex("00000000000000000000000000000000")


def main():
    hw = tbeam.detect()
    print("tbeam:", hw)
    print("lorawan module version:", lorawan.version())

    # rx2_datarate=DR_3 (SF9) is what TTN EU868 expects. For standard LoRaWAN
    # networks, omit this kwarg to use the region default (DR_0 / SF12).
    lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)

    # Restore any saved session (DevNonce, FCntUp, session keys). Must be
    # called BEFORE join on every boot.
    try:
        lw.nvram_restore()
        print("nvram restored, joined =", lw.joined())
    except OSError:
        print("no saved session — first boot")

    if not lw.joined():
        print("starting OTAA join...")
        try:
            lw.join_otaa(
                dev_eui=DEV_EUI,
                join_eui=JOIN_EUI,
                app_key=APP_KEY,
                timeout=60,
            )
            print("join OK")
        except OSError as e:
            print("join failed (timeout):", e)
            return
    else:
        print("already joined (session restored)")

    # Unconfirmed uplink at DR_0 (SF12) for maximum range on first packet.
    # After a couple of uplinks with lw.adr(True) the server will move the
    # device to a faster DR automatically.
    payload = b"hello from T-Beam"
    print(f"sending {len(payload)} B on port 1...")
    lw.send(payload, port=1, datarate=lorawan.DR_0)

    stats = lw.stats()
    print(f"tx_counter={stats['tx_counter']} "
          f"time_on_air={stats['tx_time_on_air']} ms")

    # Check for a scheduled downlink (non-blocking poll).
    pkt = lw.recv(timeout=0)
    if pkt:
        data, port, rssi, snr, multicast = pkt
        print(f"downlink port={port} rssi={rssi} snr={snr}: {data!r}")

    # Persist FCntUp so the next reboot doesn't replay a frame counter.
    lw.nvram_save()
    print("nvram saved")


if __name__ == "__main__":
    main()
