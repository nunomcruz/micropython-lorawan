MicroPython + LoRaWAN (T-Beam)
==============================

[![Flash firmware](https://img.shields.io/badge/Flash_firmware-from_browser-blue?logo=googlechrome&logoColor=white)](https://nunomcruz.github.io/micropython-lorawan/)

Fork of [MicroPython](https://micropython.org/) v1.29.0-preview with a full LoRaWAN MAC stack for LILYGO TTGO T-Beam boards.

## Contents

- [What this fork adds](#what-this-fork-adds)
- [Supported hardware](#supported-hardware)
- [Hardware setup](#hardware-setup)
- [Build](#build)
- [Quick start](#quick-start)
- [API reference](#api-reference) — see the [Method index](#method-index) for every `LoRaWAN` method
- [Usage patterns](#usage-patterns)
- [Raw LoRa — `tbeam` module](#raw-lora--tbeam-module) — see the [Function index](#function-index)
- [Hardware notes (confirmed on device)](#hardware-notes-confirmed-on-device)
- [Standards compliance](#standards-compliance)
- [Project status](#project-status)
- [Architecture](#architecture)
- [Implementation notes](#implementation-notes)
- [Releasing](#releasing)
- [Related repositories](#related-repositories)
- [License](#license)

## What this fork adds

A `USER_C_MODULE` that wraps [Semtech LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0), providing:

- OTAA and ABP join (LoRaWAN 1.0.4 and 1.1)
- Uplink and downlink, confirmed and unconfirmed
- Adaptive Data Rate, TX-power control, NVS session persistence
- Class A, Class B (beacons + ping slots) and Class C
- MAC extensions: DeviceTimeReq, LinkCheckReq, ReJoin
- Application-layer packages: Clock Sync (port 202), Remote Multicast Setup (port 200), Fragmentation / FUOTA (port 201)
- Local and remotely-provisioned multicast (up to 4 groups)

All exposed via a single `import lorawan` module.

Single firmware image for all T-Beam variants (v0.7–v1.2 including the 433 MHz edition). Hardware is auto-detected at boot; no separate builds required. The 433 MHz edition requires a firmware compiled with EU433 support — see [Regions](#regions).

Default region: EU868 (TTN). All LoRaWAN regions supported as opt-in build flags.

## Supported hardware

| T-Beam | Radio | PMU | GPS RX/TX |
|--------|-------|-----|-----------|
| v0.7 | SX1276 | None (TP4054) | 12/15 |
| v1.0 | SX1276 | AXP192 | 34/12 |
| v1.1 | SX1276 or SX1262 | AXP192 | 34/12 |
| v1.2 | SX1276 or SX1262 | AXP2101 | 34/12 |
| v1.2 (433 MHz) | SX1278 or SX1262 | AXP2101 | 34/12 |

## Hardware setup

### LoRa antenna

**Attach a matched 868 MHz (or 433 MHz) antenna to the SMA/IPEX connector before powering the board.** Transmitting without an antenna can damage the SX1276 / SX1262 PA within a few frames. The T-Beam ships with the MCU and radio soldered together — the only wiring step you need to do is screw on the antenna.

Do not reuse a 2.4 GHz (WiFi/BLE) antenna — the mismatch wastes 10+ dB of output power.

### GPS antenna

Most T-Beams come with a passive patch antenna connected via U.FL. If you are using the GPS at all, leave it attached — GPS RF is not shared with LoRa and there is no risk from running the radio without it, but your first fix will be slow without an antenna.

### Power

USB-C (or micro-USB on v0.7) powers the MCU and charges the 18650 cell. For deep-sleep sensor nodes, insert a charged 18650 and rely on the PMU to maintain the radio between wakeups.

### Registering the device on The Things Network

1. Create a TTN application at [console.cloud.thethings.network](https://console.cloud.thethings.network/).
2. Add an **end device** → "Enter end device specifics manually".
   - Frequency plan: `Europe 863–870 MHz (SF9 for RX2 — recommended)` — this is what the default `rx2_datarate=DR_3` corresponds to.
   - LoRaWAN version: `MAC V1.0.4` (or `MAC V1.1` if you passed `lorawan_version=lorawan.V1_1`).
   - Regional Parameters version: `RP002 Regional Parameters 1.0.3 revision A` (or whichever the console recommends for your MAC version).
3. Activation mode: **OTAA** (recommended). TTN will generate the JoinEUI, DevEUI and AppKey — copy these into your script.
4. After your first successful uplink the device status on TTN switches from *Never seen* to *Connected*. Send a downlink from the **Messaging → Downlink** tab to verify the RX path.

ABP is supported but discouraged on TTN: you lose replay protection across reboots unless you call `nvram_save()` after every uplink and `nvram_restore()` on every boot.

### Bench testing — antenna SWR and pre-compliance (`tx_cw`)

Before deploying a new board or after changing the antenna connector, verify the RF path with a continuous-wave transmission:

```python
import lorawan

lw = lorawan.LoRaWAN(lorawan.EU868)

# Transmit a CW carrier at 868.1 MHz, 14 dBm for 5 seconds.
# Connect a spectrum analyser or RTL-SDR; you should see a clean spike
# at 868.100 MHz. SWR > 3:1 shows up as frequency pulling or reduced
# output power on a power-meter.
lw.tx_cw(868_100_000, 14, 5)

lw.deinit()
```

**API:**  `lw.tx_cw(freq_hz, power_dbm, duration_s)`
- `freq_hz`: carrier frequency in Hz. Must be within the region band (EU868: 863–870 MHz, EU433: 433.05–434.79 MHz).
- `power_dbm`: TX power in dBm. Capped at hardware limit (SX1276: 20 dBm via PA_BOOST; SX1262: 22 dBm).
- `duration_s`: how long to transmit, 1–30 seconds. The MAC's internal timer fires `mlme_confirm(MLME_TXCW)` at the end; `tx_cw()` blocks until then.

`tx_cw()` hijacks the radio for its entire duration. Call it only when the MAC is idle (non-joined, or Class A idle between TX/RX cycles). Do **not** call it while a join or uplink is in progress.

For FCC/CE pre-compliance scans, step across the band in 200 kHz increments and note the peak power at each frequency:

```python
import lorawan, time

lw = lorawan.LoRaWAN(lorawan.EU868)
lw.duty_cycle(False)   # disable duty-cycle gating for bench use

for freq in range(863_100_000, 870_000_000, 200_000):
    print(f"CW at {freq/1e6:.1f} MHz")
    lw.tx_cw(freq, 20, 2)
    time.sleep_ms(200)

lw.deinit()
```

## Build

```bash
source micropython-esp-idf/export.sh    # set up ESP-IDF toolchain

cd micropython-lorawan
make -C mpy-cross                        # first time only (or after upstream changes)

cd ports/esp32
make submodules                          # first time only (or after IDF bump)
make BOARD=LILYGO_TTGO_TBEAM \
     USER_C_MODULES=$(pwd)/../../lorawan-module/micropython.cmake
```

Flash with `make deploy PORT=/dev/ttyUSB0` or `esptool.py` directly.

### Regions

By default only EU868 is compiled in. To enable additional regions, pass `-DLORAWAN_REGIONS=` during cmake configuration (before `make`):

```bash
# Example: EU868 + EU433 + US915 + AS923 in one firmware
cmake -B ports/esp32/build-LILYGO_TTGO_TBEAM \
      -DLORAWAN_REGIONS="EU868;EU433;US915;AS923" \
      -DBOARD=LILYGO_TTGO_TBEAM \
      -DUSER_C_MODULES=$(pwd)/lorawan-module/micropython.cmake
make -C ports/esp32 BOARD=LILYGO_TTGO_TBEAM \
     USER_C_MODULES=$(pwd)/lorawan-module/micropython.cmake
```

All region constants (`lorawan.EU868`, `lorawan.US915`, etc.) are always exported regardless of which regions are compiled in. Passing a region that was not compiled in raises `OSError` during `LoRaWAN()` construction.

| Constant  | Band          | Typical LNS         | Extra firmware |
|-----------|---------------|---------------------|----------------|
| `EU868`   | 863–870 MHz   | TTN, Helium         | (default)      |
| `EU433`   | 433–434 MHz   | private gateways    | +2 KB          |
| `US915`   | 902–928 MHz   | TTN, Helium, AWS    | +5 KB          |
| `AU915`   | 915–928 MHz   | TTN AU              | +5 KB          |
| `AS923`   | 920–925 MHz   | TTN AS, KPN         | +2 KB          |
| `KR920`   | 920–923 MHz   | private KR          | +2 KB          |
| `IN865`   | 865–867 MHz   | private IN          | +2 KB          |
| `RU864`   | 864–870 MHz   | private RU          | +2 KB          |
| `CN470`   | 470–510 MHz   | private CN          | +2 KB          |
| `CN779`   | 779–787 MHz   | private CN          | +2 KB          |

US915 and AU915 share a common 72-channel base layer (`RegionBaseUS.c`); enabling either one adds it automatically.

The T-Beam v1.2 433 MHz edition ships with either an SX1278 or an SX1262. Both are fully supported: the SX1262 variant auto-detects normally; the SX1278 is register-compatible with the SX1276 driver and auto-detects the same way. In both cases, compile with `EU433` — no other changes are needed.

Note: `tx_power()` and `tx_power_to_dbm` semantics now depend on region — the dBm ↔ MAC index conversion uses the region's actual `DEFAULT_MAX_EIRP` (see table in `tx_power()` below).

---

## Quick start

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868)

# Restore previous session if any; must be called before join on every boot.
try:
    lw.nvram_restore()
except OSError:
    pass

if not lw.joined():
    lw.join_otaa(
        dev_eui=bytes.fromhex("70B3D57ED0000000"),
        join_eui=bytes.fromhex("0000000000000000"),
        app_key=bytes.fromhex("00000000000000000000000000000000"),
        timeout=60,
    )

lw.send(b"hello", port=1)
lw.nvram_save()
```

Ready-made scripts live in [`lorawan-module/examples/`](lorawan-module/examples/) — `basic_otaa.py`, `basic_abp.py`, `sensor_node.py`, `time_sync.py`, `class_b_beacon.py`, `multicast_receiver.py`.

---

## API reference

### Method index

Quick reference — every method on the `LoRaWAN` instance plus the module-level functions of `lorawan`, with full signatures. Click the section name for details and examples.

| Method | Description |
|---|---|
| **[Join](#join)** | |
| `lw.join_otaa(dev_eui, join_eui, app_key, *, timeout=30, nwk_key=None)` | OTAA join (LoRaWAN 1.0.x or 1.1) |
| `lw.join_abp(dev_addr, nwk_s_key, app_s_key)` | ABP activation with pre-provisioned session |
| `lw.joined()` → `bool` | Whether the device is currently joined |
| **[Uplink](#uplink)** | |
| `lw.send(data, *, port=1, confirmed=False, datarate=lorawan.DR_0, timeout=120)` | Send an uplink frame |
| `lw.on_send_done(callback)` / `lw.on_send_done(None)` | Register / clear async TX-done callback |
| **[Downlink](#downlink)** | |
| `lw.recv(*, timeout=10)` → `(bytes, int, int, int, bool) \| None` | Block waiting for a downlink |
| `lw.on_recv(callback)` / `lw.on_recv(None)` | Register / clear async RX callback |
| **[Configuration](#configuration)** | |
| `lw.datarate([dr])` → `int \| None` | Get/set default DR for uplinks |
| `lw.adr([enabled])` → `bool \| None` | Get/set ADR (Adaptive Data Rate) |
| `lw.tx_power([dbm])` → `int \| None` | Get/set TX power in dBm EIRP |
| `lw.antenna_gain([gain])` → `float \| None` | Get/set antenna gain in dBi |
| `lw.battery_level([level])` → `int \| None` | Get/set DevStatus battery level (0–255) |
| `lw.stats()` → `dict` | Snapshot of last RX/TX RSSI, SNR, counters, ToA |
| **[Duty cycle](#duty-cycle)** | |
| `lw.duty_cycle([enabled])` → `bool \| None` | Get/set MAC duty-cycle enforcement |
| `lw.time_until_tx()` → `int` | ms until the next allowed TX |
| **[Persistence](#persistence)** | |
| `lw.nvram_save()` | Persist DevNonce, FCntUp and session keys to NVS |
| `lw.nvram_restore()` | Restore session from NVS (call before `join_*` on every boot) |
| **[Device class](#device-class-a--b--c)** | |
| `lw.device_class([cls])` → `int \| None` | Get/set LoRaWAN device class A/B/C |
| `lw.on_class_change(callback)` / `lw.on_class_change(None)` | Register / clear class-switch callback |
| **[Class B](#class-b-beacon-tracking)** | |
| `lw.ping_slot_periodicity([n])` → `int \| None` | Get/set ping slot periodicity (0=128 s … 7=1 s) |
| `lw.on_beacon(callback)` / `lw.on_beacon(None)` | Register / clear beacon-event callback |
| `lw.beacon_state()` → `dict \| None` | Last beacon RX info (state, time, freq, RSSI, SNR) |
| **[Time synchronisation](#time-synchronisation)** | |
| `lw.request_device_time()` | Request server time via MLME DeviceTimeReq |
| `lw.synced_time()` → `int \| None` | GPS epoch seconds from the last time sync |
| `lw.on_time_sync(callback)` / `lw.on_time_sync(None)` | Register / clear time-sync callback |
| `lw.clock_sync_enable()` → `bool` | Enable Clock Sync application package (port 202) |
| `lw.clock_sync_request(*, datarate=None)` | Send AppTimeReq via Clock Sync package |
| **[Link quality](#link-quality)** | |
| `lw.link_check(*, send_now=False, datarate=None, port=1, confirmed=False)` → `dict \| None` | Send LinkCheckReq, return margin / gateway count |
| **[LoRaWAN 1.1 rejoin](#lorawan-11-rejoin)** | |
| `lw.rejoin(type=0)` | Send a Rejoin frame (type 0/1/2) |
| **[Channel management](#channel-management)** | |
| `lw.add_channel(index, frequency, dr_min, dr_max)` | Add a custom channel |
| `lw.remove_channel(index)` | Remove a custom channel |
| `lw.channels()` → `list[dict]` | List active channels |
| **[Multicast](#multicast)** | |
| `lw.multicast_add(group, addr, mc_nwk_s_key, mc_app_s_key, *, f_count_min=0, f_count_max=0xFFFFFFFF)` → `int` | Provision a multicast group |
| `lw.multicast_rx_params(group, device_class, frequency, datarate, *, periodicity=0)` | Configure multicast RX parameters (Class B / C) |
| `lw.multicast_remove(group)` | Remove a multicast group |
| `lw.multicast_list()` → `list[dict]` | List provisioned multicast groups |
| `lw.remote_multicast_enable()` → `bool` | Enable Remote Multicast Setup package (port 200) |
| `lw.derive_mc_keys(addr, mc_root_key, *, group=0)` → `(nwk_s_key, app_s_key)` | Derive McNwkSKey / McAppSKey from McRootKey |
| **[Fragmentation (FUOTA)](#fragmentation-fuota)** | |
| `lw.fragmentation_enable(buffer_size, *, on_progress=None, on_done=None)` | Enable Fragmented Data Block transport (port 201) |
| `lw.fragmentation_data()` → `bytes \| None` | Reassembled buffer once transfer completes |
| **[Advanced MIB parameters](#advanced-mib-parameters)** | |
| `lw.nb_trans([n])` → `int \| None` | Get/set unconfirmed-uplink retransmission count |
| `lw.public_network([enabled])` → `bool \| None` | Get/set public-network sync word |
| `lw.net_id([value])` → `int \| None` | Get/set NetID |
| `lw.channel_mask([mask], *, default=False)` → `int \| tuple[int, int] \| None` | Get/set channels mask |
| `lw.rx_clock_drift(*, max_rx_error_ms=None, min_rx_symbols=None)` → `dict \| None` | Tune RX-window timing tolerance |
| `lw.rx_window_timing(*, rx1_delay_ms=None, rx2_delay_ms=None, join_accept_delay_1_ms=None, join_accept_delay_2_ms=None, max_rx_window_duration_ms=None)` → `dict \| None` | Tune RX1 / RX2 / Join-accept delays |
| `lw.rejoin_cycle(type, [cycle_seconds])` → `int \| None` | Get/set periodic Rejoin interval (LoRaWAN 1.1) |
| **[Diagnostics](#diagnostics)** | |
| `lw.frame_counters()` → `dict` | Current FCntUp / FCntDown values |
| `lw.last_error()` → `dict \| None` | Last MAC / radio error info |
| **[Lifecycle](#lifecycle)** | |
| `lw.deinit()` | Tear down the LoRaWAN task and release SPI / NVS |
| **[Module-level](#module-level)** | |
| `lorawan.LoRaWAN(region, **kwargs)` | Create and initialise the LoRaWAN stack |
| `lorawan.version()` → `str` | Module version string |
| `lorawan.test_hal()` → `dict` | Run a HAL self-test (radio, SPI, IRQ, BUSY) |

### `lorawan.LoRaWAN(region, **kwargs)`

Creates and initialises the LoRaWAN stack. Only one instance may exist at a time; reset the board to create a new one.

```python
lw = lorawan.LoRaWAN(
    region=lorawan.EU868,           # required — EU868 or EU433

    # --- LoRaWAN protocol ---
    lorawan_version=lorawan.V1_0_4, # V1_0_4 (default) or V1_1
    device_class=lorawan.CLASS_A,   # CLASS_A (default), CLASS_B, CLASS_C

    # --- RX2 window ---
    # Unset fields fall through to the region's PHY_DEF_RX2 (DR_0 / region default freq).
    # For TTN EU868, set rx2_datarate=DR_3 (the 869.525 MHz freq is already the region default).
    rx2_datarate=None,              # e.g. lorawan.DR_3 for TTN EU868
    rx2_freq=None,                  # e.g. 869525000 (Hz)

    # --- TX power ---
    # In dBm EIRP. None = region maximum (16 dBm for EU868).
    # Values within the regulatory limit go through the MAC stack.
    # Values above the limit activate a hardware override (user responsibility).
    #   SX1276 hardware cap: 20 dBm
    #   SX1262 hardware cap: 22 dBm
    tx_power=None,

    # --- Antenna gain ---
    # In dBi. Default 0.0 so the radio emits the full EIRP.
    # Set to the measured gain of your antenna if you care about staying
    # under the regulatory EIRP cap.
    antenna_gain=0.0,

    # --- Radio selection ---
    # None = auto-detect by reading SX1276 version register (default).
    # Use "sx1276" or "sx1262" to override auto-detection.
    radio=None,

    # --- Pin overrides (T-Beam defaults shown) ---
    # Only needed for non-T-Beam hardware. -1 = keep T-Beam default.
    spi_id=2,   # SPI host: 1=HSPI, 2=VSPI
    mosi=27,
    miso=19,
    sclk=5,
    cs=18,
    reset=23,
    irq=26,     # SX1276: DIO0; SX1262: DIO1
    busy=32,    # SX1262 only
)
```

---

### Join

#### `lw.join_otaa(dev_eui, join_eui, app_key, *, timeout=30, nwk_key=None)`

Initiates OTAA join. Blocks until the join accept arrives or `timeout` seconds elapse. Raises `OSError(ETIMEDOUT)` on timeout. On success, DevNonce is automatically saved to NVS.

```python
# LoRaWAN 1.0.x (TTN, ChirpStack default)
lw.join_otaa(
    dev_eui=bytes.fromhex("70B3D57ED0000000"),
    join_eui=bytes.fromhex("0000000000000000"),
    app_key=bytes.fromhex("00000000000000000000000000000000"),
    timeout=60,
)

# LoRaWAN 1.1 — separate NwkKey and AppKey
lw = lorawan.LoRaWAN(region=lorawan.EU868, lorawan_version=lorawan.V1_1)
lw.join_otaa(
    dev_eui=bytes.fromhex("70B3D57ED0000000"),
    join_eui=bytes.fromhex("0000000000000000"),
    app_key=bytes.fromhex("00000000000000000000000000000000"),
    nwk_key=bytes.fromhex("11111111111111111111111111111111"),
)
```

#### `lw.join_abp(dev_addr, nwk_s_key, app_s_key)`

Configures ABP session. Completes immediately (no over-the-air exchange).

```python
lw.join_abp(
    dev_addr=0x260B1234,
    nwk_s_key=bytes.fromhex("00000000000000000000000000000000"),
    app_s_key=bytes.fromhex("00000000000000000000000000000000"),
)
```

#### `lw.joined()` → `bool`

Returns `True` if the device has an active session (ABP always returns `True` after `join_abp`; OTAA returns `True` after a successful join or after `nvram_restore()` with a saved session).

---

### Uplink

#### `lw.send(data, *, port=1, confirmed=False, datarate=lorawan.DR_0, timeout=120)`

Transmits an uplink frame. Blocks until TX is complete (includes waiting for RX1/RX2 windows) or until `timeout` seconds elapse.

**Exceptions:**
- `OSError(EBUSY)` — the regional duty cycle is currently restricting TX. The frame was **not** queued; call `lw.time_until_tx()` to get the wait in milliseconds, sleep, then retry.
- `OSError(EIO, "send: event_status=N")` — the radio/network layer reported a failure after transmission (TX_TIMEOUT, RX1/RX2_TIMEOUT, MIC_FAIL, ADDRESS_FAIL, etc.). `N` is the `LORAMAC_EVENT_INFO_STATUS_*` code from `McpsConfirm`.
- `OSError(EIO, "send: status=N")` — the MAC API rejected the request before transmission (e.g. payload too large for current DR, MAC busy). `N` is the `LoRaMacStatus_t` code.
- Call `lw.last_error()` immediately after either `EIO` form for the full diagnostic dict.
- `OSError(ETIMEDOUT)` — `timeout` elapsed before TX completed.

`timeout` defaults to 120 s. Pass `timeout=None` to block until TX completes (non-positive ints are treated the same way).

```python
import errno, time

lw.send(b"hello")                               # unconfirmed, port 1, DR_0 (SF12)
lw.send(b"ping", confirmed=True)                # confirmed uplink — waits for ACK
lw.send(b"data", port=2, datarate=lorawan.DR_5) # SF7 — faster, shorter range
lw.send(b"slow", timeout=None)                  # block until TX completes

# Handle duty-cycle restriction
try:
    lw.send(b"payload")
except OSError as e:
    if e.args[0] == errno.EBUSY:
        time.sleep_ms(lw.time_until_tx())
        lw.send(b"payload")   # retry after the band clears
    else:
        raise
```

#### `lw.on_send_done(callback)` / `lw.on_send_done(None)`

Registers a callback fired after each uplink cycle. For confirmed uplinks, `success=True` means the network ACKed. For unconfirmed, `success=True` means the frame was transmitted.

```python
lw.on_send_done(lambda ok: print("tx done, ack:", ok))
lw.on_send_done(None)  # deregister
```

---

### Downlink

#### `lw.recv(*, timeout=10)` → `(bytes, int, int, int, bool) | None`

Blocks up to `timeout` seconds for a downlink. Returns `(data, port, rssi, snr, multicast)` or `None`. `multicast` is `True` when the frame was received on a multicast address. Use `timeout=0` to poll without blocking.

```python
pkt = lw.recv(timeout=10)
if pkt:
    data, port, rssi, snr, multicast = pkt
    print(f"rx port={port} rssi={rssi} snr={snr}: {data}")
```

#### `lw.on_recv(callback)` / `lw.on_recv(None)`

Registers a callback fired on each downlink. Callback receives five separate positional args: `data`, `port`, `rssi`, `snr`, `multicast`. `multicast` is `True` when the frame was received on a multicast address.

```python
def on_downlink(data, port, rssi, snr, multicast):
    kind = "mcast" if multicast else "ucast"
    print(f"{kind} port={port} rssi={rssi} snr={snr}: {data}")

lw.on_recv(on_downlink)
lw.on_recv(None)  # deregister
```

#### `recv()` vs `on_recv` — pick one, enforced

`recv()` and `on_recv` are mutually exclusive. Calling `recv()` while an `on_recv` callback is registered raises `RuntimeError("on_recv is registered; recv() disabled")`. Deregister first with `lw.on_recv(None)` if you need to switch modes.

| Pattern | Right for |
|---------|-----------|
| `recv(timeout=N)` | Request/response flows: send an uplink, then block waiting for the reply. |
| `on_recv(callback)` | Event-driven flows: Class C continuous listen, sensor node sleeping between uplinks. |

For Class C in particular, downlinks can arrive at any time; `on_recv` is the right choice:

```python
# Class C — correct pattern: on_recv only
lw.on_recv(on_downlink)
lw.device_class(lorawan.CLASS_C)
# recv() must not be called while on_recv is registered
```

---

### Configuration

#### `lw.datarate([dr])` → `int | None`

Getter/setter for the uplink data rate. Only meaningful when ADR is off. Returns current DR index.

```python
lw.datarate(lorawan.DR_5)  # set SF7/125kHz
lw.datarate()              # → 5
```

#### `lw.adr([enabled])` → `bool | None`

Getter/setter for Adaptive Data Rate. When ADR is enabled the network adjusts DR and TX power automatically. Enabling ADR clears any active TX power hardware override.

```python
lw.adr(True)   # enable ADR
lw.adr()       # → True
```

#### `lw.tx_power([dbm])` → `int | None`

Getter/setter for TX power in **dBm EIRP**.

Values within the region's regulatory limit go through the MAC stack (ADR-compatible):

```python
lw.tx_power(14)  # 14 dBm — within EU868 limit
lw.tx_power(10)  # 10 dBm
lw.tx_power()    # → 10
```

Values above the regulatory limit activate a **hardware override** (bypasses MAC validation). The MAC stack is unaware; ADR is automatically disabled:

```python
# SX1262: up to 22 dBm
lw.tx_power(20)  # "above region limit (16 dBm), user responsibility"
lw.tx_power(22)  # hardware maximum for SX1262

# SX1276: up to 20 dBm
lw.tx_power(20)  # hardware maximum for SX1276
```

The setter rounds down to the nearest available step (never exceeds the requested value). The table is region-aware:

| Region | Max dBm (index 0) | Step | Min dBm |
|--------|-------------------|------|---------|
| EU868  | 16                | 2    | 2       |
| EU433  | 12                | 2    | 2       |
| US915  | 30                | 2    | 2       |
| AU915  | 30                | 2    | 2       |
| AS923  | 16                | 2    | 2       |
| KR920  | 14                | 2    | 2       |
| IN865  | 30                | 2    | 10      |
| RU864  | 16                | 2    | 2       |
| CN470  | 19                | 2    | 5       |
| CN779  | 12                | 2    | 2       |

```python
lw = lorawan.LoRaWAN(region=lorawan.EU868)
lw.tx_power()    # → 16  (EU868 max)

lw = lorawan.LoRaWAN(region=lorawan.EU433)
lw.tx_power()    # → 12  (EU433 max, not 16)

lw = lorawan.LoRaWAN(region=lorawan.US915)
lw.tx_power()    # → 30  (US915 max)
```

#### `lw.antenna_gain([gain])` → `float | None`

Getter/setter for the antenna gain in dBi. The MAC subtracts this from the requested EIRP to compute the radio output power. Default is `0.0` so the radio emits the full EIRP — with a 2.15 dBi antenna the radiated power is then 2.15 dB over target.

```python
lw.antenna_gain(2.15)  # stated gain of the stock T-Beam whip
lw.antenna_gain()      # → 2.15
```

#### `lw.battery_level([level])` → `int | None`

Getter/setter for the battery value reported to the network in `DevStatusAns`. Whenever the network sends a `DevStatusReq` MAC command, the stack answers with this byte plus the SNR of the last uplink. No Python callback fires — the value is read atomically on demand by the MAC task.

| Value | Meaning |
|-------|---------|
| `0` | Device is powered by an external source (USB/DC) |
| `1..254` | Battery level (`1` = empty, `254` = full) |
| `255` | Unable to measure (default) |

```python
lw.battery_level(0)          # on USB
lw.battery_level(1 + int(pct * 253 / 100))  # 0–100 % → 1..254
lw.battery_level(255)        # unknown
lw.battery_level()           # → current value
```

See [`examples/sensor_node.py`](lorawan-module/examples/sensor_node.py) for a worked example that reads the AXP192 battery voltage and maps it to the LoRaWAN range.

#### `lw.stats()` → `dict`

Returns a snapshot of runtime statistics:

```python
lw.stats()
# {
#   'rssi': -109,              # last downlink RSSI (dBm)
#   'snr': 6,                  # last downlink SNR (dB)
#   'last_tx_fcnt_up': 4,      # FCntUp from the last successful uplink
#   'rx_counter': 1,           # downlink frame counter
#   'tx_time_on_air': 991,     # last uplink time on air (ms)
#   'last_tx_ack': True,       # confirmed uplink: True if ACKed
#   'last_tx_dr': 3,           # DR used for the last uplink (DR_0..DR_7)
#   'last_tx_freq': 868100000, # frequency of the last uplink (Hz)
#   'last_tx_power': 14,       # TX power of the last uplink (dBm EIRP)
# }
```

`last_tx_dr`, `last_tx_freq`, and `last_tx_power` all default to 0 before the first uplink (`last_tx_fcnt_up == 0`). `last_tx_power` is in dBm EIRP, consistent with `tx_power()`.

`last_tx_fcnt_up` mirrors the MAC FCntUp captured from the most recent **successful** `mcps_confirm`. It does not advance on failed uplinks, so after a string of failures the value can lag the live counter inside the MAC.

---

### Duty cycle

EU868 and EU433 enforce a regional duty cycle (typically 1 % air-time per band). The MAC honours this by default — `EU868_DUTY_CYCLE_ENABLED = 1` in the region file — and every uplink past the first is gated by the regional band timers.

**Immediate rejection.** When the band is restricted, `send()` raises `OSError(EBUSY)` immediately — the frame is **not** queued inside the MAC. Use `time_until_tx()` to get the remaining wait in milliseconds:

```python
import errno, time

try:
    lw.send(b"data")
except OSError as e:
    if e.args[0] == errno.EBUSY:
        time.sleep_ms(lw.time_until_tx())
        lw.send(b"data")   # retry once the band is clear
    else:
        raise
```

#### `lw.duty_cycle([enabled])` → `bool | None`

Getter/setter for the regional duty-cycle gate. **Disabling is non-conformant on public ISM bands** and intended only for bench testing on a private gateway you own. The setter logs a warning when called with `False`.

```python
lw.duty_cycle()        # → True on EU868 / EU433
lw.duty_cycle(False)   # private gateway / bench only
```

#### `lw.time_until_tx()` → `int`

Returns the milliseconds remaining until the next uplink is allowed. Derived from `DutyCycleWaitTime` captured on the most recent `send()`. Returns `0` before the first send, when duty cycle is disabled, or once the wait window has elapsed.

---

### Persistence

The MAC session (DevAddr, session keys, frame counters, ADR state, DevNonce) is stored in ESP32 NVS so the device resumes after a reboot without re-joining.

**Critical boot sequence:**

```python
lw = lorawan.LoRaWAN(region=lorawan.EU868)

try:
    lw.nvram_restore()  # must be called before join on every boot
except OSError:
    pass  # first boot — no saved session yet

if not lw.joined():
    lw.join_otaa(...)   # DevNonce auto-saved to NVS after successful join

lw.send(b"data")
lw.nvram_save()         # persist FCntUp after each uplink
```

**Why `nvram_restore()` must come first:** the MAC initialises DevNonce to 0 on every boot. Without restore, each reboot repeats already-seen nonces and the network server rejects the join with "devnonce is too small".

#### `lw.nvram_save()`

Saves the full MAC context to NVS. Call after each uplink to keep frame counters safe across reboots.

#### `lw.nvram_restore()`

Restores a previously saved session from NVS. Raises `OSError(ENOENT)` if no session is saved.

---

### Device class (A / B / C)

#### `lw.device_class([cls])` → `int | None`

Getter/setter for the device class.

- **Getter** (no argument): returns the current class (`CLASS_A`, `CLASS_B` or `CLASS_C`).
- **Setter** (with `cls`): requests a transition to `CLASS_A`, `CLASS_B` or `CLASS_C`.
  - Class A ↔ Class C: synchronous — `MIB_DEVICE_CLASS` is set immediately and `on_class_change(new_class)` fires before the call returns.
  - Class B: asynchronous and multi-step. Requires a prior time sync (see `request_device_time()` below). The call returns immediately once beacon acquisition starts; the actual class change is signalled later via `on_class_change(CLASS_B)`. Beacon lock, loss and acquisition-failure events are surfaced via `on_beacon(cb)`.

Raises `OSError(EIO)` if the transition fails (setter only). Raises `RuntimeError` for state preconditions (not initialised, CLASS_B requires a joined session).


```python
lw.device_class()                  # → CLASS_A

# Class C — instantaneous
lw.device_class(lorawan.CLASS_C)

# Class B — prerequisite: time sync
lw.request_device_time()
lw.send(b"")                       # DeviceTimeReq piggy-backs on this uplink
lw.device_class(lorawan.CLASS_B)
```

#### `lw.on_class_change(callback)` / `lw.on_class_change(None)`

Registers a callback fired on every completed class transition. Callback receives the new class as a single int.

```python
lw.on_class_change(lambda c: print("now in class", c))
```

---

### Class B (beacon tracking)

#### `lw.ping_slot_periodicity([n])` → `int | None`

Getter/setter for the Class B ping-slot periodicity `N` (0..7). Period between ping slots is `2^N` seconds. Takes effect on the next `device_class(CLASS_B)` — changing it while already in Class B requires renegotiation.

```python
lw.ping_slot_periodicity(4)  # 16 s between ping slots
lw.ping_slot_periodicity()   # → 4
```

#### `lw.on_beacon(callback)` / `lw.on_beacon(None)`

Registers a callback fired on every beacon event. Callback receives two separate positional args: `state` (one of the `BEACON_*` constants) and `info` (the beacon dict from `beacon_state()`, or `None`).

```python
def on_beacon(state, info):
    if state == lorawan.BEACON_LOCKED:
        print("beacon locked:", info)
    elif state == lorawan.BEACON_LOST:
        print("beacon lost — reverting to Class A")

lw.on_beacon(on_beacon)
```

States: `BEACON_ACQUISITION_OK`, `BEACON_ACQUISITION_FAIL`, `BEACON_LOCKED`, `BEACON_NOT_FOUND`, `BEACON_LOST`.

#### `lw.beacon_state()` → `dict | None`

Returns the most recent beacon info, or `None` if no beacon has been received yet.

```python
lw.beacon_state()
# {
#   'state': 2,                # last BEACON_* code (BEACON_LOCKED = 2)
#   'time': 1398067200,        # GPS epoch seconds
#   'freq': 869525000,         # beacon frequency (Hz)
#   'datarate': 3,             # DR the beacon was received on
#   'rssi': -95,
#   'snr': 7,
#   'gw_info_desc': 0,         # gateway info descriptor
#   'gw_info': b'\x00\x00\x00\x00\x00\x00',  # 6-byte gateway info field
# }
```

---

### Time synchronisation

Two independent paths are available:

- **MAC DeviceTimeReq** (LoRaWAN 1.0.3+). Piggy-backed on the next uplink. Simpler; needed as a prerequisite for Class B.
- **Clock Sync application package** (LoRa-Alliance v1.0.0, port 202). Sends its own uplink; also handles periodic re-sync and provides drift estimation.

Both fire `on_time_sync(cb)` with the GPS epoch seconds at the moment of sync. `synced_time()` then returns the live advancing time for as long as the device stays powered.

#### `lw.request_device_time()`

Queues an MLME DeviceTimeReq. Does **not** transmit — the request piggy-backs on the next uplink.

```python
lw.request_device_time()
lw.send(b"")            # carries the DeviceTimeReq over the air
# after RX window closes, on_time_sync fires and synced_time() returns a value:
print(lw.synced_time()) # → GPS epoch seconds, or None if no answer yet
```

#### `lw.synced_time()` → `int | None`

Returns the *live* corrected GPS epoch seconds (advances with the local clock between syncs). Returns `None` if no sync has happened yet.

GPS epoch = Unix epoch − 315964800 s (Jan 6 1980). Add this offset back to convert to Unix time.

#### `lw.on_time_sync(callback)` / `lw.on_time_sync(None)`

Registers a callback fired on every successful sync. Callback receives the GPS epoch seconds as a single int.

#### `lw.clock_sync_enable()` → `bool`

Registers the `LmhpClockSync` application package (port 202). Returns `True` on success. Must be called once; safe to call before `join_otaa()`.

#### `lw.clock_sync_request(*, datarate=None)`

Sends an `AppTimeReq` on port 202 (which also piggy-backs a MAC DeviceTimeReq). Unlike `request_device_time()`, this **does transmit an uplink** — no follow-up `send()` is required. Raises `RuntimeError` if the device is not joined or the package has not been enabled.

- `datarate` (optional, `DR_0`..`DR_5`): forces `MIB_CHANNELS_DATARATE` for this single AppTimeReq. `LmhpClockSync` snapshots and restores the DR around the MCPS confirm, so the override does not leak into subsequent uplinks. Useful when ADR has ratcheted the DR up to SF7 and range-limited port-202 frames are being dropped at the gateway — pass `lorawan.DR_0` for the most robust sync.

---

### Link quality

#### `lw.link_check(*, send_now=False, datarate=None, port=1, confirmed=False)` → `dict | None`

Queues an MLME `LinkCheckReq` MAC command. Returns the most recent `LinkCheckAns` as `{"margin": N, "gw_count": N}`, or `None` if no answer has been received yet.

Two modes:

- **`send_now=False` (default)** — only queues the piggy-back. The `LinkCheckReq` rides on the **next** uplink the caller performs (`send(...)`, `clock_sync_request()`, etc.). Returns whatever is cached from a previous cycle — `None` until the first answer arrives. Cheapest mode; right when regular telemetry is already flowing.

```python
lw.link_check()               # queue the request
lw.send(b"")                  # carries LinkCheckReq over the air
result = lw.link_check()      # read the answer after RX window closes
if result:
    print(f"margin={result['margin']} dB, seen by {result['gw_count']} gateways")
```

- **`send_now=True`** — also emits an empty uplink to carry the piggy-back, waits for the TX+RX cycle to complete, and returns the fresh answer in a single call. Costs one uplink of airtime and bumps `FCntUp` by one. Raises `OSError(EIO, "send: event_status=N")` on radio/network failure, `OSError(EIO, "send: status=N")` on MAC API rejection, or `OSError(ETIMEDOUT)` on timeout.

```python
result = lw.link_check(send_now=True, datarate=lorawan.DR_0)

# Confirmed link probe on a non-default port (e.g. LNS filters port 1):
result = lw.link_check(send_now=True, port=2, confirmed=True, datarate=lorawan.DR_0)
```

- `datarate` (optional, `DR_0`..`DR_7`): only meaningful when `send_now=True`; sets the DR for that uplink. Defaults to the MAC's current `MIB_CHANNELS_DATARATE`. Pass `DR_0` for range-critical probes when ADR may have ratcheted the DR up to SF7.
- `port` (optional, 1..223, default 1): application port for the probe uplink. Override when your LNS filters port 1.
- `confirmed` (optional, default `False`): send the probe as a confirmed uplink. Doubles as a keep-alive that requires an ACK.

---

### LoRaWAN 1.1 rejoin

#### `lw.rejoin(type=0)`

Sends a ReJoin-request frame. Only meaningful on LoRaWAN 1.1. Raises `RuntimeError` if not joined, `ValueError` if `type` is outside 0–2.

- `type=0`: announce presence; network may refresh session keys.
- `type=1`: periodic rejoin, carries DevEUI+JoinEUI; a Join-Accept may follow.
- `type=2`: request session-key refresh.

Unlike `link_check()` / `request_device_time()` (piggy-back), a rejoin is itself an uplink — no follow-up `send()` is required.

---

### Channel management

By default the EU868 region starts with three mandatory channels (868.1 / 868.3 / 868.5 MHz). With OTAA, TTN sends five additional channels (867.1 / 867.3 / 867.5 / 867.7 / 867.9 MHz) in the CFList of the Join-Accept. With ABP those channels are never provisioned automatically — add them manually after `join_abp()`.

#### `lw.add_channel(index, frequency, dr_min, dr_max)`

Adds or replaces a MAC channel. Indexes 0–2 are the three EU868 default channels protected by the MAC (attempting to overwrite them raises `OSError(EIO, "add_channel: status=N")`). Use indexes 3–15 for extra channels.

```python
# Add the 5 extra TTN EU868 channels after ABP join
extra = [867100000, 867300000, 867500000, 867700000, 867900000]
for i, freq in enumerate(extra, start=3):
    lw.add_channel(index=i, frequency=freq, dr_min=0, dr_max=5)
```

#### `lw.remove_channel(index)`

Disables a channel and clears it from the region channel mask. Indexes 0–2 are protected and cannot be removed.

```python
lw.remove_channel(index=7)
```

#### `lw.channels()` → `list[dict]`

Returns all currently active channels. Each dict has `index`, `frequency` (Hz), `dr_min`, and `dr_max`.

```python
lw.channels()
# [{'index': 0, 'frequency': 868100000, 'dr_min': 0, 'dr_max': 5},
#  {'index': 1, 'frequency': 868300000, 'dr_min': 0, 'dr_max': 5},
#  {'index': 2, 'frequency': 868500000, 'dr_min': 0, 'dr_max': 5},
#  {'index': 3, 'frequency': 867100000, 'dr_min': 0, 'dr_max': 5},
#  ...]
```

---

### Multicast

Up to 4 multicast groups (indices 0–3) can be active simultaneously. Multicast frames are received on either the Class C continuous RX window or the Class B ping slots, depending on the group's RX params.

#### `lw.multicast_add(group, addr, mc_nwk_s_key, mc_app_s_key, *, f_count_min=0, f_count_max=0xFFFFFFFF)` → `int`

Provisions a local multicast group. Returns the `group` index that was registered. Keys must be supplied out-of-band (shared with the network server). Use `remote_multicast_enable()` below for server-driven provisioning.

```python
idx = lw.multicast_add(
    group=0,
    addr=0x11223344,
    mc_nwk_s_key=bytes.fromhex("01" * 16),
    mc_app_s_key=bytes.fromhex("02" * 16),
    f_count_min=0,
    f_count_max=0xFFFFFFFF,
)
# idx == 0
```

#### `lw.multicast_rx_params(group, device_class, frequency, datarate, *, periodicity=0)`

Configures the multicast RX window. `device_class` must be `CLASS_B` or `CLASS_C`. `periodicity` is only used for Class B (0–7 → 2^N seconds).

```python
# Class C multicast — continuous listen on 869.525 MHz / SF9
lw.multicast_rx_params(
    group=0,
    device_class=lorawan.CLASS_C,
    frequency=869525000,
    datarate=lorawan.DR_3,
)

# Class B multicast — ping slot every 32 s
lw.multicast_rx_params(
    group=0,
    device_class=lorawan.CLASS_B,
    frequency=869525000,
    datarate=lorawan.DR_3,
    periodicity=5,
)
```

#### `lw.multicast_remove(group)`

Removes a group and clears its crypto state.

#### `lw.multicast_list()` → `list[dict]`

Returns the active groups with their metadata. `addr` is an int (use `hex(g['addr'])` or `f"0x{g['addr']:08X}"` to display it). RX-param keys (`device_class`, `frequency`, `datarate`, `periodicity`) are only present after `multicast_rx_params()` has been called.

```python
lw.multicast_list()
# [{'group': 0, 'addr': 0x11223344, 'is_remote': False,
#   'f_count_min': 0, 'f_count_max': 4294967295,
#   'device_class': 2, 'frequency': 869525000, 'datarate': 0}]
```

#### `lw.remote_multicast_enable()` → `bool`

Registers the `LmhpRemoteMcastSetup` package (port 200). After this, the network server can provision groups remotely via `McGroupSetupReq` / `McGroupClassCSessionReq` / `McGroupClassBSessionReq` — no further Python glue required. The package drives the class switch automatically when a session starts.

#### Key derivation — `lw.derive_mc_keys(addr, mc_root_key, *, group=0)` → `(nwk_s_key, app_s_key)`

Derives `McNwkSKey` and `McAppSKey` from `McKey` (the 16-byte multicast group key) for a given `DevAddr` and group slot. Returns a 2-tuple of `bytes` objects.

This replaces the manual AES derivation that callers previously had to do before calling `multicast_add()`. The MAC performs the derivation using the same `LoRaMacCryptoDeriveMcSessionKeyPair` function used internally by the remote-setup flow, so the result is spec-compliant.

```python
mc_key = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
mc_addr = 0x00000000

nwk_s_key, app_s_key = lw.derive_mc_keys(mc_addr, mc_key, group=0)
print("McNwkSKey:", nwk_s_key.hex())
print("McAppSKey:", app_s_key.hex())

# Pass the derived keys directly to multicast_add()
lw.multicast_add(
    group=0,
    addr=mc_addr,
    mc_nwk_s_key=nwk_s_key,
    mc_app_s_key=app_s_key,
)
```

**Parameters:**
- `addr`: 32-bit multicast DevAddr.
- `mc_root_key`: 16-byte `McKey` (the multicast group key from which session keys are derived).
- `group` (keyword, default 0): group slot 0–3. Each slot has independent session keys; calling `derive_mc_keys` for a slot also stores the key material in the soft secure element for that group.

> **Note:** In LoRaWAN 1.1.1 terminology `mc_root_key` corresponds to `McKey`, not `McRootKey`. `McRootKey` is the root used in the server-device key-exchange flow managed by `LmhpRemoteMcastSetup`. If you are using remote multicast setup, the server handles key derivation automatically and you do not need to call `derive_mc_keys()` at all.

---

### Fragmentation (FUOTA)

Reassembles a firmware image delivered via multicast, with recovery of up to `redundancy` fragments via Reed–Solomon.

#### `lw.fragmentation_enable(buffer_size, *, on_progress=None, on_done=None)`

Registers `LmhpFragmentation` (port 201) and allocates a flat RAM reassembly buffer.

- `on_progress(counter, nb, size, lost)` — fragment counter, total fragments, fragment size (bytes), fragments lost so far. Four separate positional args.
- `on_done(status, size)` — `status=0` means success; `status>0` is the number of unrecoverable missing fragments. Two separate positional args.

```python
def on_progress(counter, nb, size, lost):
    print(f"frag {counter}/{nb} size={size} lost={lost}")

def on_done(status, size):
    if status == 0:
        data = lw.fragmentation_data()[:size]
        print(f"FUOTA complete: {size} bytes")
    else:
        print(f"FUOTA incomplete: {status} fragments missing")

lw.fragmentation_enable(buffer_size=32 * 1024,
                        on_progress=on_progress,
                        on_done=on_done)
```

#### `lw.fragmentation_data()` → `bytes | None`

Returns the reassembled buffer (full size — slice down to the `size` reported by `on_done`). Returns `None` if fragmentation was never enabled.

---

### Advanced MIB parameters

Thin wrappers over LoRaMAC-node's MIB. Most users won't need these — the defaults match the LoRaWAN spec and TTN. Useful for tuning reliability (`nb_trans`), running on private networks (`public_network`), recovering from RTC drift after deepsleep (`rx_clock_drift`), and diagnostics.

#### `lw.nb_trans([n])` → `int | None`

Per-uplink retransmission count (1..15) for unconfirmed frames. The MAC default is 1; ADR may also raise this via `LinkADRReq`. Higher values trade airtime for reliability when ADR cannot raise output power (e.g. already at region max). Wraps `MIB_CHANNELS_NB_TRANS`.

```python
lw.nb_trans()       # → 1
lw.nb_trans(3)      # send each unconfirmed uplink up to 3×
```

#### `lw.public_network([enabled])` → `bool | None`

Selects the LoRa sync word: `True` (default) for public networks (TTN, ChirpStack, Helium), `False` (sync word `0x12`) for private deployments. Toggling re-arms the radio so the new sync word applies to the next RX/TX cycle. Wraps `MIB_PUBLIC_NETWORK`.

```python
lw.public_network()        # → True
lw.public_network(False)   # private LoRaWAN (sync word 0x12)
```

#### `lw.net_id([value])` → `int | None`

24-bit network identifier (`0..0xFFFFFF`). After OTAA this reflects the JoinAccept's NetID. The setter is meant for ABP, where the value is otherwise zero; setting it on an OTAA session has no effect once the network has assigned its own. Wraps `MIB_NET_ID`.

#### `lw.channel_mask([mask], *, default=False)` → `int | tuple[int, int] | None`

16-bit bitmask of enabled channels (bit `i` = channel `i`). EU868 default is `0x0007` (channels 0..2). Pass `default=True` to also update `MIB_CHANNELS_DEFAULT_MASK` so a subsequent `ResetMacParameters` does not reinstate the region default. Getter with `default=True` returns `(current, default)`.

```python
lw.channel_mask()              # → 0x0007
lw.channel_mask(default=True)  # → (0x0007, 0x0007)
lw.channel_mask(0x00FF)        # enable channels 0..7
```

#### `lw.rx_clock_drift(*, max_rx_error_ms=None, min_rx_symbols=None)` → `dict | None`

Tolerances for RX1/RX2 window timing. Useful after long deepsleep when the RTC has drifted relative to the gateway. Getter (no kwargs) returns `{max_rx_error_ms, min_rx_symbols}`; setter applies only the kwargs that were passed.

| Field | Description |
|-------|-------------|
| `max_rx_error_ms` | wall-clock drift the MAC tolerates when timing the RX windows (the window opens earlier and stays longer to compensate). Wraps `MIB_SYSTEM_MAX_RX_ERROR`. |
| `min_rx_symbols` | radio's preamble-detect symbol count (1..255). Higher = more tolerant of slow gateways at the cost of receiver power. Wraps `MIB_MIN_RX_SYMBOLS`. |

```python
# After waking from a 30-minute deepsleep with no RTC sync:
lw.rx_clock_drift(max_rx_error_ms=200)
```

#### `lw.rx_window_timing(*, rx1_delay_ms=None, rx2_delay_ms=None, join_accept_delay_1_ms=None, join_accept_delay_2_ms=None, max_rx_window_duration_ms=None)` → `dict | None`

RX/Join-accept delay timings, all in milliseconds. LoRaWAN-spec defaults are RX1 1000 ms, RX2 2000 ms, JA1 5000 ms, JA2 6000 ms. The network overrides RX1/RX2 via the JoinAccept; setting them is mainly useful for ABP or for matching a private-network gateway with non-standard timing. Wraps `MIB_RECEIVE_DELAY_1/2`, `MIB_JOIN_ACCEPT_DELAY_1/2`, `MIB_MAX_RX_WINDOW_DURATION`. Getter returns a dict with all five fields.

#### `lw.rejoin_cycle(type, [cycle_seconds])` → `int | None`

LoRaWAN 1.1 periodic rejoin cycles. `type` is `0` (Type 0 rejoin — roaming/keep-alive context) or `1` (Type 1 — full re-keying with new JoinNonce). Cycle in seconds. Wraps `MIB_REJOIN_0_CYCLE` and `MIB_REJOIN_1_CYCLE`.

> **Note:** LoRaMAC-node v4.7.0 documents a `MIB_REJOIN_2_CYCLE` but does not include it in the actual `MibParamType` enum. `type=2` is rejected with `ValueError`.

```python
lw.rejoin_cycle(0)              # → current Type 0 cycle in seconds
lw.rejoin_cycle(0, 86400)       # rejoin every 24 h
```

### Diagnostics

#### `lw.frame_counters()` → `dict`

Live snapshot of the frame counters from the Crypto NVM context. Returns `{fcnt_up, fcnt_down, n_fcnt_down, a_fcnt_down}`. LoRaMAC-node v4.7.0 exposes no direct MIB getter for these — this reads `MIB_NVM_CTXS` and copies the relevant fields out.

| Field | Description |
|-------|-------------|
| `fcnt_up` | uplink counter to be sent in the next frame |
| `fcnt_down` | single downlink counter (1.0.x) |
| `n_fcnt_down` | network-frame downlink counter (LoRaWAN 1.1) |
| `a_fcnt_down` | application-frame downlink counter (LoRaWAN 1.1) |

Useful for diagnosing dessynchronisation with the network. `stats()["last_tx_fcnt_up"]` is cached at TX time; `frame_counters()["fcnt_up"]` is the live MAC value.

#### `lw.last_error()` → `dict | None`

Returns a dict describing the most recent MAC-layer failure, or `None` if no failure has been recorded. Fields:

| Key | Type | Description |
|-----|------|-------------|
| `loramac_status` | `int` | `LoRaMacStatus_t` from the failing API call (0 = OK; see `LoRaMac.h`) |
| `event_status` | `int` | `LoRaMacEventInfoStatus_t` from `McpsConfirm`/`MlmeConfirm` (0 = OK) |
| `context` | `str` | Which API failed: `"send"`, `"join"`, `"multicast_add"`, `"add_channel"`, etc. |
| `epoch_us` | `int` | `esp_timer_get_time()` at the moment of failure (µs since boot) |

When `event_status != 0`, the MAC-layer event code is the most informative field. Common values: `1 = TX_TIMEOUT`, `2 = RX1_TIMEOUT`, `3 = RX2_TIMEOUT`, `4 = MIC_FAIL`, `5 = ADDRESS_FAIL`, `6 = JOIN_FAIL`. When `event_status == 0` but `loramac_status != 0`, the API itself rejected the call (e.g. `LORAMAC_STATUS_BUSY = 1`, `LORAMAC_STATUS_MC_GROUP_UNDEFINED = 22`).

```python
import errno

try:
    lw.send(b"hello")
except OSError as e:
    if e.args[0] == errno.EIO:
        err = lw.last_error()
        print(f"TX failed: context={err['context']} "
              f"event_status={err['event_status']} loramac_status={err['loramac_status']}")
        if err['event_status'] != 0:
            # Radio/network-layer failure from McpsConfirm/MlmeConfirm.
            # Branch on LoRaMacEventInfoStatus_t values:
            if err['event_status'] == 3:   # RX2_TIMEOUT — missed downlink window
                pass
            elif err['event_status'] == 4: # MIC_FAIL — key mismatch
                pass
        else:
            # MAC API rejected the call before transmission.
            # Branch on LoRaMacStatus_t values:
            if err['loramac_status'] == 1:  # LORAMAC_STATUS_BUSY
                pass
            elif err['loramac_status'] == 11: # LORAMAC_STATUS_LENGTH_ERROR
                pass
```

### Lifecycle

The LoRaWAN stack owns a FreeRTOS task, the SPI bus, DIO interrupt handlers and an `esp_timer` — resources that live outside the MicroPython heap. Tearing them down cleanly matters when the REPL's soft-reset (`Ctrl-D`) runs, or when you want to re-create the object with different parameters.

#### `lw.deinit()`

Ordered teardown, strict order: (1) deregister DIO ISRs first so no in-flight interrupt can dispatch into half-torn-down MAC state; (2) stop the MAC (`LoRaMacDeInitialization`, falling back to a radio sleep if a TX/RX is in flight — SPI is still up at this point so the sleep command reaches the radio); (3) drop LmHandler package registrations and any FUOTA buffer; (4) release the SPI bus, delete the `esp_timer` backing the MAC timer list, and let the LoRaWAN task self-delete. Idempotent — safe to call twice.

If the MAC ever stalls past the 2 s bounded wait on teardown, the Python side logs a warning and leaves the internal command queue / rx queue / event group in place rather than free them while the task may still be using them — a small leak is strictly preferable to a use-after-free.

Calling `lorawan.LoRaWAN(...)` when an instance already exists automatically tears down the previous one — no explicit `deinit()` required. This matches the behaviour of `machine.SPI` and other MicroPython peripherals that simply reconfigure on re-creation.

```python
lw = lorawan.LoRaWAN(region=lorawan.EU868)
# ...
# Re-create with different parameters — previous instance is torn down automatically.
lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)
```

#### Soft-reset safety

`Ctrl-D` in the REPL triggers a MicroPython soft-reset: the VM heap is swept, `__main__` is re-imported, but the ESP32 port does **not** restart the MCU. Without a hook, the FreeRTOS task keeps running against a freed `lorawan_obj_t` — the next DIO interrupt or timer callback dereferences garbage and crashes.

`deinit()` is registered as `__del__` on the type, and the `lorawan_obj_t` is allocated with a finaliser, so the GC sweep that runs during soft-reset (`gc_sweep_all()` in the ESP32 port's `main.c`) fires `__del__ → deinit()` automatically. This matches the pattern used by `machine.I2S`, `machine.I2CTarget`, `ssl` etc. No user action required — `Ctrl-D` is safe.

Hard-reset (power cycle, `machine.reset()`) is always safe: the radio comes up from POR.

#### Context manager

`__enter__` returns `self`; `__exit__` calls `deinit()` regardless of exception state. Use `with` when you want teardown guaranteed at scope exit:

```python
with lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3) as lw:
    lw.nvram_restore() if lw.joined() else lw.join_otaa(...)
    lw.send(b"data")
    lw.nvram_save()
# lw.deinit() has run here — task is gone, SPI bus is free
```

---

### Module-level

#### `lorawan.version()` → `str`

Returns the module version string, e.g. `'0.16.0'`.

#### `lorawan.test_hal()` → `dict`

Low-level HAL smoke test (SPI reg 0x42 probe + 200 ms timer). Useful for bring-up on unfamiliar hardware before calling `LoRaWAN()`.

---

### Constants

```python
# Regions — all always exported; passing an uncompiled region raises OSError at init
lorawan.EU868   # 863–870 MHz (Europe, default)
lorawan.EU433   # 433–434 MHz (Europe, opt-in)
lorawan.US915   # 902–928 MHz (Americas, opt-in)
lorawan.AU915   # 915–928 MHz (Australia, opt-in)
lorawan.AS923   # 920–925 MHz (Asia, opt-in)
lorawan.KR920   # 920–923 MHz (Korea, opt-in)
lorawan.IN865   # 865–867 MHz (India, opt-in)
lorawan.RU864   # 864–870 MHz (Russia, opt-in)
lorawan.CN470   # 470–510 MHz (China, opt-in)
lorawan.CN779   # 779–787 MHz (China, opt-in)

# LoRaWAN protocol versions
lorawan.V1_0_4  # LoRaWAN 1.0.4 — single root key, single-key MIC (default)
lorawan.V1_1    # LoRaWAN 1.1   — separate NwkKey/AppKey, two-key MIC

# Device classes
lorawan.CLASS_A  # Class A — uplink-triggered RX windows (default)
lorawan.CLASS_B  # Class B — scheduled ping slots
lorawan.CLASS_C  # Class C — continuous RX2

# Data rates (EU868): higher index = higher SF = longer range, slower
lorawan.DR_0   # SF12 / 125 kHz — maximum range
lorawan.DR_1   # SF11 / 125 kHz
lorawan.DR_2   # SF10 / 125 kHz
lorawan.DR_3   # SF9  / 125 kHz
lorawan.DR_4   # SF8  / 125 kHz
lorawan.DR_5   # SF7  / 125 kHz — maximum throughput (LoRa)
lorawan.DR_6   # SF7  / 250 kHz — double bandwidth variant
lorawan.DR_7   # FSK  / 50 kbps — highest throughput, short range

# Beacon states (first arg to on_beacon callback)
lorawan.BEACON_ACQUISITION_OK
lorawan.BEACON_ACQUISITION_FAIL
lorawan.BEACON_LOCKED
lorawan.BEACON_NOT_FOUND
lorawan.BEACON_LOST
```

---

## Usage patterns

### OTAA sensor node (TTN, standard setup)

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)

try:
    lw.nvram_restore()
except OSError:
    pass

if not lw.joined():
    lw.join_otaa(
        dev_eui=bytes.fromhex("70B3D57ED0000000"),
        join_eui=bytes.fromhex("0000000000000000"),
        app_key=bytes.fromhex("00000000000000000000000000000000"),
        timeout=60,
    )

lw.send(b"hello", port=1)
lw.nvram_save()
```

### ABP with ADR

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)

try:
    lw.nvram_restore()
except OSError:
    lw.join_abp(
        dev_addr=0x260B1234,
        nwk_s_key=bytes.fromhex("00000000000000000000000000000000"),
        app_s_key=bytes.fromhex("00000000000000000000000000000000"),
    )

lw.adr(True)
lw.send(b"data", port=1)
lw.nvram_save()
```

### Custom TX power

```python
import lorawan

# 14 dBm — within EU868 regulatory limit, MAC-managed
lw = lorawan.LoRaWAN(region=lorawan.EU868, tx_power=14)

# 20 dBm — hardware override, beyond EU868 limit (user responsibility)
# SX1276 cap: 20 dBm. SX1262 cap: 22 dBm.
lw = lorawan.LoRaWAN(region=lorawan.EU868, tx_power=20)

# Adjust after init
lw.tx_power(12)   # regulatory path
lw.tx_power(22)   # hardware override (SX1262 only; SX1276 caps at 20)
lw.tx_power()     # → current value in dBm
```

### Callbacks (async pattern)

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_datarate=lorawan.DR_3)
# ... join ...

def on_downlink(data, port, rssi, snr, mc):
    print(f"rx port={port} rssi={rssi} snr={snr} mcast={mc}: {data}")

lw.on_recv(on_downlink)
lw.on_send_done(lambda ok: print("tx ack:", ok))

lw.send(b"ping", confirmed=True)
```

### Non-T-Beam hardware (custom pins)

```python
import lorawan

lw = lorawan.LoRaWAN(
    region=lorawan.EU868,
    radio="sx1262",
    spi_id=2,
    mosi=23, miso=19, sclk=18,
    cs=5,
    reset=14,
    irq=26,
    busy=25,
)
```

---

## Raw LoRa — `tbeam` module

The frozen `tbeam` module provides hardware auto-detection and access to the raw LoRa physical layer (MicroPython's `lora-sx127x` / `lora-sx126x` drivers). This is **completely separate** from the LoRaWAN MAC stack — no join, no frame counters, just raw RF packets.

> **Important:** `tbeam.lora_modem()` and `lorawan.LoRaWAN()` both own the SPI bus and the radio hardware. They are mutually exclusive — do not use both at the same time. Reset the board to switch between them.

### Function index

| Function | Description |
|---|---|
| **[Hardware detection](#hardware-detection)** | |
| `tbeam.detect()` → `HardwareInfo` | Probe SPI / I2C / UART, return detected hardware |
| **[Raw LoRa TX/RX](#raw-lora-txrx)** | |
| `tbeam.lora_modem(hw)` → `SyncModem` | Create raw `lora-sx127x` / `lora-sx126x` modem |
| **[Other helpers](#other-tbeam-helpers)** | |
| `tbeam.gps_uart(hw)` → `machine.UART` | UART pre-configured for the on-board GPS |
| `tbeam.i2c_bus()` → `machine.I2C` | Hardware I2C at 400 kHz on SDA=21, SCL=22 |
| `tbeam.lora_spi(baudrate=10_000_000)` → `machine.SPI` | SPI bus configured for the LoRa radio |
| `tbeam.lora_pins(hw)` → `tuple` | `(cs, irq, rst)` for SX1276 or `(cs, irq, rst, busy)` for SX1262 |

### Hardware detection

```python
import tbeam

hw = tbeam.detect()
# HardwareInfo(radio='sx1262', pmu='axp192', irq=33, busy=32,
#              gps_rx=34, gps_tx=12, oled=False)
```

`detect()` probes SPI, I2C, and optionally the GPS UART. Cache the result if calling multiple times — it takes 2–4 s on the first call.

### Raw LoRa TX/RX

```python
import tbeam

hw  = tbeam.detect()
lm  = tbeam.lora_modem(hw)   # SX1276 or SX1262 SyncModem

lm.configure({
    "freq_khz":    868100,
    "sf":          7,
    "bw":          "125",
    "coding_rate": 5,
    "output_power": 14,
    "preamble_len": 8,
    "crc_en":      True,
    "implicit_header": False,
})

lm.send(b"hello world")

pkt = lm.recv(timeout_ms=5000)
if pkt:
    print("rx:", pkt, "rssi:", lm.last_rssi)
```

### Other `tbeam` helpers

```python
uart = tbeam.gps_uart(hw)          # machine.UART, 9600 baud
i2c  = tbeam.i2c_bus()             # machine.I2C at 400 kHz
spi  = tbeam.lora_spi(baudrate=10_000_000)
pins = tbeam.lora_pins(hw)         # (cs, irq, rst) or (cs, irq, rst, busy)
```

---

## Hardware notes (confirmed on device)

**SX1262 TCXO (DIO3, 1.8V).** The T-Beam SX1262 uses an internal TCXO via DIO3. The HAL calls `SX126xSetDio3AsTcxoCtrl(TCXO_CTRL_1_8V, 640)` (timeout 640 ticks ≈ 10 ms) as the first operation in `SX126xIoInit()`. Without it: `XOSC_START_ERR (0x20)`. Lazy image calibration on the first `SetRfFrequency` is sufficient — no need for a full `SX126xCalibrate(0x7F)` at boot.

**Harmless `XOSC_START_ERR (0x0020)`.** The first TCXO startup briefly trips this bit in the device error register. The bit is sticky (cleared only by `ClearDeviceErrors` or a power-on reset), so it's still set on subsequent commands, but it has no effect on operation. Don't use the device error register as a health check on its own.

**SX1262 DIO2 as RF switch.** DIO2 controls the TX/RX antenna switch. Enabled via `SX126xSetDio2AsRfSwitchCtrl(true)`.

**SX1262 BUSY pin polled before every SPI transaction.** All `SX126xWriteCommand` / `SX126xReadCommand` paths call `SX126xWaitOnBusy()` first. After a sleep cycle (`RadioSleep(WarmStart=1)` after RX1/RX2), BUSY drops LOW prematurely, so the HAL also calls an `ensure_awake()` helper (which issues `GetStatus` and updates `operating_mode = MODE_STDBY_RC`) before each `WaitOnBusy` — otherwise a dropped wakeup race can clock garbage into the radio. The SPI mutex is recursive so the wakeup SPI does not deadlock when invoked while the lock is already held.

**SX1276 uses a crystal, not TCXO.** `REG_LR_TCXO = 0x09`. Setting 0x19 (TCXO mode) silences the radio.

**SPI bus exclusivity.** The HAL owns the SPI bus exclusively. Do not create a `machine.SPI` instance on the same bus while the LoRaWAN stack is running. Mixing them on the SX1262 causes RC13M + ADC calibration failures (`OpError 0xa00`).

**TX power hardware maximums.** The SX1262 outputs up to +22 dBm; the SX1276 (PA_BOOST) up to +20 dBm. Both are well above the EU868 regulatory EIRP limit of 16 dBm. Going above 16 dBm requires a hardware TX-power override (see `tx_power()` above) and is the user's responsibility. Reaching +30 dBm requires an external PA (e.g. Ebyte E22-900M30S module); the software hook point is `SX126xAntSwOn()` / `SX126xAntSwOff()` in `sx126x_board.c`.

**RX2 default.** The MAC uses the region's `PHY_DEF_RX2` unless overridden via `rx2_datarate=` / `rx2_freq=`. TTN EU868 wants `rx2_datarate=DR_3` (SF9) — 869.525 MHz is already the region default. Standard LoRaWAN specifies DR_0 (SF12) on the same frequency.

---

## Standards compliance

The `LmhpCompliance` package (port 224) required by the LoRa Alliance certification test suite is registered on demand:

```python
import lorawan

lw = lorawan.LoRaWAN(lorawan.EU868)
lw.join_otaa(...)          # device must be joined before certification runs
lw.compliance_enable()     # register port-224 DUT command handler
# The certification tool now drives the device via downlinks on port 224.
# No further Python intervention is needed during the test sequence.
```

The package handles: `PKG_VERSION_ANS`, `DUT_RESET`, `DUT_JOIN`, `SWITCH_CLASS`, `ADR_BIT_CHANGE`, `DUTY_CYCLE_CTRL`, `TX_PERIODICITY_CHANGE`, `TX_FRAMES_CTRL`, `ECHO_PAYLOAD`, `RX_APP_CNT`, `LINK_CHECK`, `DEVICE_TIME`, `PING_SLOT_INFO`, `BEACON_RX_STATUS_IND`, `TX_CW`, `DUT_FPORT_224_DISABLE`, and `DUT_VERSION`.

> Full LoRa Alliance end-device certification additionally needs a gateway, a network server running the certification profile, and the official certification test tool. The firmware itself provides all required DUT-side protocol support.

## Project status

**Stable — v1.5.0.** The Python API is frozen since v1.0.0; minor versions since have been additive. All major functionality is implemented and tested on hardware:

- All T-Beam variants (v0.7 through v1.2 and the 433 MHz edition) auto-detected at boot — single firmware image.
- OTAA and ABP join (LoRaWAN 1.0.4 and 1.1).
- Class A, Class B (beacons + ping slots) and Class C.
- Confirmed and unconfirmed uplinks/downlinks, ADR, NVS session persistence, lifecycle (`deinit`, soft-reset safe, context manager).
- MAC commands: DeviceTimeReq, LinkCheckReq, ReJoin, TXCW, DevStatusAns (with `battery_level`).
- Application packages: Clock Sync (port 202), Remote Multicast Setup (port 200), Fragmentation / FUOTA (port 201), Compliance (port 224).
- Multicast — local and remote provisioning, up to 4 groups, Class B and Class C reception.
- All 10 LoRaWAN regions (build-time opt-in beyond the EU868 default).
- Continuous-wave transmission (`tx_cw`) for SWR checks and FCC/CE pre-compliance scans.
- Typed exceptions throughout; `last_error()` accessor for post-mortem diagnostics.
- Advanced MIB tuning: `nb_trans`, `public_network`, `net_id`, `channel_mask`, `rx_clock_drift`, `rx_window_timing`, `rejoin_cycle`, `frame_counters`.

For the full development history and version-by-version changes, see [CHANGELOG.md](CHANGELOG.md).

## Architecture

```
lorawan-module/
├── micropython.cmake       build entry point (INTERFACE library)
├── bindings/
│   ├── modlorawan.c        Python bindings + FreeRTOS task
│   └── lmhandler_shim.c    minimal LmHandler adapter for app-layer packages
├── hal/
│   ├── esp32_gpio.c        GPIO (ISR, edge interrupt)
│   ├── esp32_spi.c         SPI master (ESP-IDF spi_master)
│   ├── esp32_timer.c       LoRaWAN timers (esp_timer, sorted list)
│   ├── esp32_delay.c       DelayMs
│   ├── esp32_board.c       BoardInit, random seed, unique ID
│   ├── sx1276_board.c      SX1276 radio HAL
│   ├── sx126x_board.c      SX1262 radio HAL (TCXO, DIO2 RF switch)
│   ├── sx1276_radio_wrapper.c  symbol isolation (dual-radio build)
│   ├── sx126x_radio_wrapper.c
│   ├── radio_select.c      runtime Radio vtable selection
│   └── pin_config.c        runtime pin config + TX-power override
├── config/
│   └── lorawan_config.h    T-Beam defaults, region flags
├── examples/               see examples/README.md
└── loramac-node/           Semtech LoRaMAC-node v4.7.0 (copied sources)
    ├── src/mac/            LoRaMac.c, regions, LoRaMacClassB.c
    ├── src/radio/          SX1276, SX126x drivers
    ├── src/peripherals/    soft-se (AES, CMAC)
    └── src/apps/.../packages/   LmhpClockSync, LmhpRemoteMcastSetup, LmhpFragmentation
```

The LoRaWAN MAC runs in a dedicated FreeRTOS task on CPU1 (priority 6, 8 KB stack). Python communicates via a command queue and event group. All `mp_printf` / MicroPython object allocation is confined to `mp_sched_schedule` trampolines running on the Python thread.

## Implementation notes

Quirks that bit hard during the port and are worth knowing if you contribute or fork.

**ESP-IDF v5+ stack depth is in BYTES, not words.** `xTaskCreatePinnedToCore` and `xTaskCreate` in ESP-IDF v5 take a byte count (vanilla FreeRTOS uses words). `STACK_SIZE / sizeof(StackType_t)` gives 2 048 instead of 8 192 — the LoRaWAN task needs ≥ 8 192 bytes; `LoRaMacInitialization()` → `SX1276Init()` → `RxChainCalibration()` is a deep call chain.

**`pdMS_TO_TICKS(n)` can evaluate to 0.** With `CONFIG_FREERTOS_HZ=100` (10 ms tick), `pdMS_TO_TICKS(2) = 0`. Passing 0 to `ulTaskNotifyTake` makes it non-blocking, which produces a tight loop that acquires/releases `xKernelLock` at MHz rate, starving the other CPU's `xQueueSend` of the same lock. Use `pdMS_TO_TICKS(10)` (= 1 tick) as the minimum meaningful sleep.

**Silent stack overflow on a busy-spinning task.** `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY=1` only checks the canary at context switch. A task that never blocks (e.g. busy-spin with 0-timeout `ulTaskNotifyTake`) never gets context-switched, so stack corruption is silent — symptom is "task starts, prints first line, then never prints again" while it actually executes garbage from a corrupted stack.

**`mp_printf` from a non-Python FreeRTOS task deadlocks.** `mp_hal_stdout_tx_strn` calls `MP_THREAD_GIL_ENTER()` for strings above `MICROPY_PY_STRING_TX_GIL_THRESHOLD`. If the LoRaWAN task doesn't own the GIL (and the Python thread holds it while sleeping in `xEventGroupWaitBits`), the C task blocks forever. Use `esp_rom_printf` (ROM UART, no locking, ISR-safe) for diagnostics from the LoRaWAN task.

**MicroPython QSTR scanner doesn't see user-module compile definitions.** The QSTR scanner generates `genhdr/qstrdefs.generated.h` with a fixed set of preprocessor flags that does **not** include `target_compile_definitions` set on `usermod_lorawan` (e.g. `-DREGION_EU868`). Any `MP_QSTR_*` constant inside an `#ifdef REGION_X` block is silently missing from the header, producing an "undeclared" compile error the moment it appears in a ROM table outside a function. Rule: never guard QSTR-bearing constants with `#ifdef`. Region constants (`EU868`, `US915`, …) are exported unconditionally; passing an uncompiled region fails cleanly at `LoRaMacInitialization()`.

**Stale per-file `.qstr` after adding new QSTRs.** If you see `MP_QSTR_XXX undeclared`, delete `genhdr/qstr/<path-mangled>.modlorawan.c.qstr`, `genhdr/qstrdefs.collected.h{,.hash}`, `genhdr/qstrdefs.generated.h` and `frozen_content.c` inside `ports/esp32/build-LILYGO_TTGO_TBEAM/`, then rebuild.

**Changing `LORAWAN_REGIONS` after first configure.** It's a CMake `CACHE STRING`, so passing `-DLORAWAN_REGIONS=...` to `make` does not override the cache. Re-run `cmake -S ports/esp32 -B ports/esp32/build-LILYGO_TTGO_TBEAM -DLORAWAN_REGIONS="..." -DBOARD=... -DUSER_C_MODULES=...` first, then `make`.

## Releasing

The embedded firmware version comes from `git describe`. Without a `*-lorawan` tag, `git describe` falls back to upstream's `v1.29.0-preview` and the build is indistinguishable from stock MicroPython. So tag **before** the build:

```bash
git tag -a v1.0.X-lorawan -m "Release v1.0.X-lorawan"
git push origin v1.0.X-lorawan
# now run the canonical build — the embedded version becomes v1.0.X-lorawan
# (or v1.0.X-lorawan-N-gHASH if there are commits after the tag, which is intentional)
bash scripts/release.sh v1.0.X-lorawan
```

`scripts/release.sh` verifies the build output, copies the three `.bin` files to the `gh-pages` branch, updates `manifest.json` (web flasher), and creates a GitHub Release with the binaries attached. Version scheme: `v<major>.<minor>.<patch>-lorawan` — patch for bug fixes, minor for additive features.

## Related repositories

| Repository | Purpose |
|---|---|
| [LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0) | Semtech's LoRaWAN MAC stack |
| [MicroPython](https://github.com/micropython/micropython) | Upstream |

## License

MicroPython is licensed under the MIT license. LoRaMAC-node is licensed under the Revised BSD License. See [LICENSE](LICENSE) for details.
