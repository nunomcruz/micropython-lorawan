MicroPython + LoRaWAN (T-Beam)
==============================

[![Flash firmware](https://img.shields.io/badge/Flash_firmware-from_browser-blue?logo=googlechrome&logoColor=white)](https://nunomcruz.github.io/micropython-lorawan/)

Fork of [MicroPython](https://micropython.org/) v1.29.0-preview with a full LoRaWAN MAC stack for LILYGO TTGO T-Beam boards.

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

Single firmware image for all T-Beam variants (v0.7–v1.2, SX1276 and SX1262 radios). Hardware is auto-detected at boot; no separate builds required.

Primary region: EU868 (TTN). EU433 supported.

## Supported hardware

| T-Beam | Radio | PMU | GPS RX/TX |
|--------|-------|-----|-----------|
| v0.7 | SX1276 | None (TP4054) | 12/15 |
| v1.0 | SX1276 | AXP192 | 34/12 |
| v1.1 | SX1276 or SX1262 | AXP192 | 34/12 |
| v1.2 | SX1276 or SX1262 | AXP2101 | 34/12 |

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
- `RuntimeError("send failed")` — the MAC reported a TX/RX failure (TX_TIMEOUT, RX1/RX2_TIMEOUT, MIC_FAIL, ADDRESS_FAIL, etc.). The exception type will switch to `OSError(EIO, "send: event_status=N")` in v1.1; see [TODO.md](TODO.md) Session 21. Callers should catch both for forward compatibility.
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

#### `lw.on_tx_done(callback)` / `lw.on_tx_done(None)`

Registers a callback fired after each uplink cycle. For confirmed uplinks, `success=True` means the network ACKed. For unconfirmed, `success=True` means the frame was transmitted.

```python
lw.on_tx_done(lambda ok: print("tx done, ack:", ok))
lw.on_tx_done(None)  # deregister
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

#### `lw.on_rx(callback)` / `lw.on_rx(None)`

Registers a callback fired on each downlink. Callback receives five separate positional args: `data`, `port`, `rssi`, `snr`, `multicast`. `multicast` is `True` when the frame was received on a multicast address.

```python
def on_downlink(data, port, rssi, snr, multicast):
    kind = "mcast" if multicast else "ucast"
    print(f"{kind} port={port} rssi={rssi} snr={snr}: {data}")

lw.on_rx(on_downlink)
lw.on_rx(None)  # deregister
```

#### `recv()` vs `on_rx` — pick one

Both read from the same internal queue. **`recv()` has priority**: while it is blocking, the MicroPython VM cannot run scheduled callbacks, so when a packet arrives `recv()` pops it and the `on_rx` trampoline runs later against an empty queue — the callback never fires for that message.

| Pattern | Right for |
|---------|-----------|
| `recv(timeout=N)` | Request/response flows: send an uplink, then block waiting for the reply. |
| `on_rx(callback)` | Event-driven flows: Class C continuous listen, sensor node sleeping between uplinks. |

Do not use both simultaneously. For Class C in particular, downlinks can arrive at any time; `on_rx` is the right choice:

```python
# Class C — correct pattern: on_rx only
lw.on_rx(on_downlink)
lw.device_class(lorawan.CLASS_C)
# recv() is never called — on_rx fires for every downlink

# Class C — incorrect pattern: mixing both
lw.on_rx(on_downlink)
lw.device_class(lorawan.CLASS_C)
pkt = lw.recv(timeout=30)  # while this blocks, on_rx is silenced
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

EU868 table (for reference):

| dBm EIRP | MAC index |
|----------|-----------|
| 16 | 0 (max) |
| 14 | 1 |
| 12 | 2 |
| 10 | 3 |
|  8 | 4 |
|  6 | 5 |
|  4 | 6 |
|  2 | 7 (min) |

The setter rounds down to the nearest available step (never exceeds the requested value).

> Note: the dBm↔index table is hardcoded to EU868 (max 16 dBm, step 2). On EU433, `tx_power()` still accepts and returns EU868 dBm values; the MAC indices are correct but the dBm annotations will be off by a few dB. Region-accurate mapping is a future refactor.

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
#   'rssi': -109,           # last downlink RSSI (dBm)
#   'snr': 6,               # last downlink SNR (dB)
#   'tx_counter': 4,        # uplink frame counter
#   'rx_counter': 1,        # downlink frame counter
#   'tx_time_on_air': 991,  # last uplink time on air (ms)
#   'last_tx_ack': True,    # confirmed uplink: True if ACKed
#   'last_tx_dr': 3,        # DR used for the last uplink (DR_0..DR_5)
#   'last_tx_freq': 868100000,  # frequency of the last uplink (Hz)
#   'last_tx_power': 14,    # TX power of the last uplink (dBm EIRP)
# }
```

`last_tx_dr`, `last_tx_freq`, and `last_tx_power` all default to 0 before the first uplink (`tx_counter == 0`). `last_tx_power` is in dBm EIRP, consistent with `tx_power()`.

`tx_counter` mirrors the MAC FCntUp captured from the most recent **successful** `mcps_confirm`. It does not advance on failed uplinks, so after a string of failures the value can lag the live counter inside the MAC. A live `frame_counters()` getter (returning `(fcnt_up, fcnt_down)` from the live `MIB_NVM_CTXS` snapshot) is planned for v1.1 — see [TODO.md](TODO.md) Session 23.

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

`request_class(cls)` is a deprecated alias for `device_class(cls)`.

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

Both update the same internal `network_time_gps` snapshot, so `on_time_sync(cb)` and `network_time()` / `synced_time()` work identically regardless of which path caused the sync.

#### `lw.request_device_time()`

Queues an MLME DeviceTimeReq. Does **not** transmit — the request piggy-backs on the next uplink.

```python
lw.request_device_time()
lw.send(b"")                # carries the DeviceTimeReq over the air
# after RX window closes:
print(lw.network_time())    # → GPS epoch seconds, or None if no answer yet
```

#### `lw.network_time()` → `int | None`

Returns the GPS epoch seconds captured at the moment of the last sync, or `None` if time has never been synced.

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

#### `lw.link_check(*, send_now=False, datarate=None)` → `dict | None`

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

- **`send_now=True`** — also emits an empty unconfirmed uplink on port 1 to carry the piggy-back, waits for the TX+RX cycle to complete, and returns the fresh answer in a single call. Costs one uplink of airtime and bumps `FCntUp` by one. Raises `RuntimeError("link_check send failed")` on MAC rejection or `OSError(ETIMEDOUT)` on timeout.

```python
result = lw.link_check(send_now=True, datarate=lorawan.DR_0)
```

- `datarate` (optional, `DR_0`..`DR_5`): only meaningful when `send_now=True`; sets the DR for that uplink. Defaults to the MAC's current `MIB_CHANNELS_DATARATE`. Pass `DR_0` for range-critical probes when ADR may have ratcheted the DR up to SF7.

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

Adds or replaces a MAC channel. Indexes 0–2 are the three EU868 default channels protected by the MAC (attempting to overwrite them raises `RuntimeError`). Use indexes 3–15 for extra channels.

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
# Regions
lorawan.EU868   # 868 MHz band (Europe)
lorawan.EU433   # 433 MHz band (Europe)

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
lorawan.DR_5   # SF7  / 125 kHz — maximum throughput

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

lw.on_rx(on_downlink)
lw.on_tx_done(lambda ok: print("tx ack:", ok))

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

**SX1262 TCXO (DIO3, 1.8V).** The T-Beam SX1262 uses an internal TCXO via DIO3. The HAL calls `SX126xSetDio3AsTcxoCtrl(TCXO_CTRL_1_8V)` as the first operation in `SX126xIoInit()`. Without it: `XOSC_START_ERR (0x20)`.

**SX1262 DIO2 as RF switch.** DIO2 controls the TX/RX antenna switch. Enabled via `SX126xSetDio2AsRfSwitchCtrl(true)`.

**SX1276 uses a crystal, not TCXO.** `REG_LR_TCXO = 0x09`. Setting 0x19 (TCXO mode) silences the radio.

**SPI bus exclusivity.** The HAL owns the SPI bus exclusively. Do not create a `machine.SPI` instance on the same bus while the LoRaWAN stack is running.

**TX power hardware maximums.** The SX1262 outputs up to +22 dBm; the SX1276 (PA_BOOST) up to +20 dBm. Both are well above the EU868 regulatory EIRP limit of 16 dBm. Going above 16 dBm requires a hardware TX-power override (see `tx_power()` above) and is the user's responsibility. Reaching +30 dBm requires an external PA (e.g. Ebyte E22-900M30S module); the software hook point is `SX126xAntSwOn()` / `SX126xAntSwOff()` in `sx126x_board.c`.

**RX2 default.** The MAC uses the region's `PHY_DEF_RX2` unless overridden via `rx2_datarate=` / `rx2_freq=`. TTN EU868 wants `rx2_datarate=DR_3` (SF9) — 869.525 MHz is already the region default. Standard LoRaWAN specifies DR_0 (SF12) on the same frequency.

---

## Project status

**v1.0.0.** Phase 1–5 of the roadmap are complete end-to-end on hardware:

- Phase 1: T-Beam board definition (all variants, runtime auto-detect).
- Phase 2: USER_C_MODULE skeleton.
- Phase 3: ESP32 HAL (GPIO, SPI, timers, board).
- Phase 4: OTAA/ABP, uplink/downlink, confirmed messages, ADR, NVS persistence.
- Phase 5: runtime pin config, LoRaWAN 1.1, RX2 / TX-power / antenna-gain control, Class C, EU433, DeviceTimeReq, LinkCheckReq, ReJoin, Clock Sync package, Class B (beacon + ping slots), multicast (local and remote), fragmentation (FUOTA).

See [TODO.md](TODO.md) for the development roadmap and per-session notes. See [CLAUDE.md](../CLAUDE.md) for project context including hardware constants and FreeRTOS pitfalls.

## Roadmap (post-1.0)

The v1.0.0 surface is stable, but a v1.0 review found a handful of consistency gaps and exposed Semtech features. Sessions 21–25 are tracked in [TODO.md](TODO.md):

- **Session 21 — Error handling and diagnostics.** Replace the generic `RuntimeError("<api> failed")` raises across `send()`, `multicast_*`, channel management, link-check and FUOTA with `OSError(EIO, "<api>: status=N")` carrying the underlying `LoRaMacStatus_t`. Add `lw.last_error()` for post-mortem (LoRaMAC status + event status + context). Append the join-failure status to the `OSError(ETIMEDOUT)` from `join_otaa`. Bumps to v1.1.
- **Session 22 — API consistency.** Drop the deprecated `request_class` alias and (likely) `network_time()`. Make `recv()` and `on_rx()` mutually exclusive at the binding level. Rename the success-only `tx_counter` in `stats()` to `last_tx_fcnt_up`. Export `DR_6` and `DR_7` constants. Parameterise `link_check(send_now=True, port=, confirmed=)`.
- **Session 23 — Expanded MIB getters/setters.** `nb_trans()` (cheapest reliability knob — multi-TX of unconfirmed uplinks), `channel_mask()`, live `frame_counters()`, `public_network()`, `rx_window_timing()`, `rx_clock_drift()`, `rejoin_cycle()`, `net_id()`. Wraps existing MAC state with no new MAC code.
- **Session 24 — New MAC primitives.** `tx_cw(freq, power, duration)` for continuous-wave bench tests and FCC/CE pre-compliance. LoRa Alliance Compliance package (LmhpCompliance) for certification testing. `derive_mc_keys(addr, mc_root_key)` so multicast keys can be derived in-MAC instead of supplied by the caller.
- **Session 25 — Additional regions.** Build-time opt-in for US915 / AU915 / AS923 / KR920 / IN865 / RU864 / CN470 / CN779. Region-aware dBm ↔ TX-power index conversion (today the EU868 table is hardcoded, so the `tx_power()` getter returns wrong dBm on EU433).

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

## Related repositories

| Repository | Purpose |
|---|---|
| [LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0) | Semtech's LoRaWAN MAC stack |
| [MicroPython](https://github.com/micropython/micropython) | Upstream |

## License

MicroPython is licensed under the MIT license. LoRaMAC-node is licensed under the Revised BSD License. See [LICENSE](LICENSE) for details.
