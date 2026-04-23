"""
basic_abp.py — minimal ABP session + uplink.

ABP skips the over-the-air join: DevAddr and session keys are provisioned on
both ends up front. Faster to bring up than OTAA, but you lose replay
protection across reboots unless you save/restore FCntUp with nvram_save()
and nvram_restore(). Most networks (TTN included) prefer OTAA.

Edit DEV_ADDR / NWK_S_KEY / APP_S_KEY with the values from your LNS console.
"""

import tbeam
import lorawan

DEV_ADDR   = 0x260B1234
NWK_S_KEY  = bytes.fromhex("00000000000000000000000000000000")
APP_S_KEY  = bytes.fromhex("00000000000000000000000000000000")


def main():
    hw = tbeam.detect()
    print("tbeam:", hw)

    lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_3)

    # Restore FCntUp across reboots. If nothing is saved yet, provision the
    # ABP session from scratch.
    try:
        lw.nvram_restore()
        print("nvram restored — frame counter preserved")
    except OSError:
        print("no saved session — provisioning ABP")
        lw.join_abp(
            dev_addr=DEV_ADDR,
            nwk_s_key=NWK_S_KEY,
            app_s_key=APP_S_KEY,
        )

    # ADR on — let the network choose the best DR once packets are flowing.
    lw.adr(True)

    payload = b"ping"
    print(f"sending {len(payload)} B, confirmed=True...")
    try:
        lw.send(payload, port=1, confirmed=True)
    except RuntimeError as e:
        print("send failed:", e)
        return

    stats = lw.stats()
    print(f"tx_counter={stats['tx_counter']} "
          f"ack={stats['last_tx_ack']} "
          f"rssi={stats['rssi']} snr={stats['snr']}")

    # Persist the new frame counter before leaving.
    lw.nvram_save()


if __name__ == "__main__":
    main()
