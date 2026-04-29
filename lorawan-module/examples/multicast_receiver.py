"""
multicast_receiver.py — join a multicast group and listen for group downlinks.

Multicast lets a single downlink from the network reach many devices at
once — the basis for large fleet firmware updates (FUOTA). The keys can be
provisioned in two ways:

  * Local: you set MC_ADDR / MC_NWK_S_KEY / MC_APP_S_KEY on the device and
    make sure the LNS knows the same values. Simpler for experimentation.
  * Remote: the LNS sends McGroupSetupReq via the LmhpRemoteMcastSetup
    package (port 200). Enable with lw.remote_multicast_enable() — the
    package then drives the class switch automatically.

This example uses the local path.

The device must be joined to a normal unicast session first (multicast frames
still ride on top of a joined device context — they don't bypass the MAC).
After the group is added and RX params configured, we switch to Class C so
the radio is continuously listening on the multicast frequency.
"""

import tbeam
import lorawan
from time import sleep

# --- Unicast credentials (for the initial OTAA join) ---
DEV_EUI  = bytes.fromhex("70B3D57ED0000000")
JOIN_EUI = bytes.fromhex("0000000000000000")
APP_KEY  = bytes.fromhex("00000000000000000000000000000000")

# --- Multicast group 0 ---
# These must match what your LNS has provisioned for the group. On TTN,
# the multicast address is a DevAddr-like 32-bit value; the keys are
# independent of any unicast device's keys.
MC_GROUP      = 0
MC_ADDR       = 0x11223344
MC_NWK_S_KEY  = bytes.fromhex("01010101010101010101010101010101")
MC_APP_S_KEY  = bytes.fromhex("02020202020202020202020202020202")

# RX window for the multicast group. Class C continuous RX2 listens on
# the same freq/DR across the whole slot. 869.525 MHz / DR_3 (SF9) is
# the standard EU868 multicast choice (it matches TTN's RX2 defaults).
MC_FREQ   = 869525000
MC_DR     = lorawan.DR_3


def on_downlink(data, port, rssi, snr, mc):
    kind = "MULTICAST" if mc else "unicast  "
    print(f"[rx] {kind} port={port} rssi={rssi} snr={snr}: {data!r}")


def main():
    hw = tbeam.detect()
    print("tbeam:", hw)

    lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)
    lw.on_rx(on_downlink)

    try:
        lw.nvram_restore()
    except OSError:
        pass

    if not lw.joined():
        print("joining OTAA (needed for multicast session)...")
        lw.join_otaa(dev_eui=DEV_EUI, join_eui=JOIN_EUI,
                     app_key=APP_KEY, timeout=60)
        lw.nvram_save()
        print("join OK")

    # --- configure the multicast group ---
    print(f"\nadding multicast group {MC_GROUP} addr=0x{MC_ADDR:08X}")
    lw.multicast_add(
        group=MC_GROUP,
        addr=MC_ADDR,
        mc_nwk_s_key=MC_NWK_S_KEY,
        mc_app_s_key=MC_APP_S_KEY,
    )

    print(f"setting RX params: freq={MC_FREQ} Hz dr={MC_DR} class=C")
    lw.multicast_rx_params(
        group=MC_GROUP,
        device_class=lorawan.CLASS_C,
        frequency=MC_FREQ,
        datarate=MC_DR,
    )

    print("groups configured:")
    for g in lw.multicast_list():
        print(f"  {g}")

    # --- switch to Class C so the radio listens continuously ---
    print("\nswitching to Class C...")
    lw.device_class(lorawan.CLASS_C)
    print(f"device_class = {lw.device_class()} (CLASS_C={lorawan.CLASS_C})")

    # --- listen forever ---
    print("\nready. Send a multicast downlink from the LNS to "
          f"addr 0x{MC_ADDR:08X}. Ctrl-C to stop.\n")
    try:
        while True:
            sleep(10)
    except KeyboardInterrupt:
        print("stopping")
        # Tidy up so the next boot starts clean.
        try:
            lw.multicast_remove(MC_GROUP)
        except (OSError, RuntimeError):
            # v1.0 raises RuntimeError on MAC failure; v1.1 will switch this
            # to OSError(EIO) (see TODO Session 21). Catching both is
            # forward-compatible.
            pass
        lw.device_class(lorawan.CLASS_A)
        lw.nvram_save()


if __name__ == "__main__":
    main()
