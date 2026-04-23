"""
sensor_node.py — periodic sensor uplink with deep sleep.

Pattern:
  1. Boot → detect hardware, bring up LoRaWAN, restore session from NVS.
  2. If there is no session, do an OTAA join (costs one airtime window).
  3. Read a "sensor" (here: uptime + supply voltage if a PMU is present —
     replace read_sensor() with your own reading).
  4. Uplink the sample on port 2.
  5. Save NVS, go to deep sleep for SLEEP_SECONDS.
  6. On wake the MCU resets — we start over from step 1 without rejoin.

Deep sleep drops the T-Beam to a few hundred µA (dominated by the PMU and
regulators, not the ESP32 core). For real low-power operation you also want
to shut down the GPS via the PMU before sleeping — not included here because
it depends on which AXP chip is fitted.
"""

import tbeam
import lorawan
import machine
import struct
import time

DEV_EUI  = bytes.fromhex("70B3D57ED0000000")
JOIN_EUI = bytes.fromhex("0000000000000000")
APP_KEY  = bytes.fromhex("00000000000000000000000000000000")

SLEEP_SECONDS = 15 * 60   # 900 s — respects TTN fair-use policy at DR_0..DR_5
UPLINK_PORT   = 2


def read_sensor(hw):
    """Gather a sample. Replace with your real sensor code.

    Returns 6 bytes:
      uint32 uptime_seconds + uint16 supply_millivolts
    """
    uptime = int(time.time())
    mv = 0

    # If an AXP192/AXP2101 is fitted, read VBus/battery voltage. We do the
    # raw I2C read here to avoid dragging in a full PMU driver. The AXP192
    # battery-voltage ADC is at 0x78/0x79 (12 bits, 1.1 mV/LSB).
    if hw.pmu == "axp192":
        try:
            i2c = tbeam.i2c_bus()
            raw = i2c.readfrom_mem(0x34, 0x78, 2)
            mv = ((raw[0] << 4) | (raw[1] & 0x0F)) * 11 // 10
        except Exception as e:
            print("pmu read failed:", e)

    return struct.pack(">IH", uptime & 0xFFFFFFFF, mv & 0xFFFF)


def ensure_joined(lw):
    """Restore session from NVS; fall back to OTAA join on first boot."""
    try:
        lw.nvram_restore()
        if lw.joined():
            print("session restored from NVS")
            return
    except OSError:
        pass

    print("no saved session — joining over the air...")
    lw.join_otaa(
        dev_eui=DEV_EUI,
        join_eui=JOIN_EUI,
        app_key=APP_KEY,
        timeout=60,
    )
    print("join OK")


def main():
    wake_reason = machine.reset_cause()
    print(f"boot (reset_cause={wake_reason})")

    hw = tbeam.detect()
    print("tbeam:", hw)

    lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_3)

    # ADR lets the server pick the most efficient DR for this node — crucial
    # for battery life on a device that sleeps most of the time.
    lw.adr(True)

    try:
        ensure_joined(lw)
    except OSError as e:
        print("join failed:", e)
        print(f"back to sleep for {SLEEP_SECONDS} s, will retry on wake")
        machine.deepsleep(SLEEP_SECONDS * 1000)

    payload = read_sensor(hw)
    print(f"payload: {payload.hex()}")

    try:
        lw.send(payload, port=UPLINK_PORT)
    except RuntimeError as e:
        # Transient failure — persist whatever state we have and sleep anyway.
        # The FCntUp isn't advanced on a failed send, so the network stays in
        # sync.
        print("send failed:", e)
    else:
        stats = lw.stats()
        print(f"tx_counter={stats['tx_counter']} "
              f"time_on_air={stats['tx_time_on_air']} ms "
              f"rssi={stats['rssi']} snr={stats['snr']}")

        pkt = lw.recv(timeout=0)
        if pkt:
            data, port, rssi, snr = pkt
            print(f"downlink port={port} rssi={rssi} snr={snr}: {data!r}")

    # Always save before sleep — otherwise FCntUp is lost and next boot
    # will either repeat a counter (rejected by the server) or need to
    # rejoin from scratch.
    lw.nvram_save()
    print(f"sleeping for {SLEEP_SECONDS} s")
    machine.deepsleep(SLEEP_SECONDS * 1000)


if __name__ == "__main__":
    main()
