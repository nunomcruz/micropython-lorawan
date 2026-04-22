MicroPython + LoRaWAN (T-Beam)
==============================

Fork of [MicroPython](https://micropython.org/) v1.29.0-preview with a full LoRaWAN MAC stack for LILYGO TTGO T-Beam boards.

## What this fork adds

A `USER_C_MODULE` that wraps [Semtech LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0), providing OTAA/ABP join, uplink/downlink, confirmed messages, ADR, and NVS session persistence. Import as `lorawan`.

Single firmware image for all T-Beam variants (v0.7–v1.2, SX1276 and SX1262 radios). Hardware is auto-detected at boot; no separate builds required.

Primary region: EU868 (TTN). EU433 supported.

## Supported hardware

| T-Beam | Radio | PMU | GPS RX/TX |
|--------|-------|-----|-----------|
| v0.7 | SX1276 | None (TP4054) | 12/15 |
| v1.0 | SX1276 | AXP192 | 34/12 |
| v1.1 | SX1276 or SX1262 | AXP192 | 34/12 |
| v1.2 | SX1276 or SX1262 | AXP2101 | 34/12 |

## Build

```bash
source micropython-esp-idf/export.sh    # set up ESP-IDF toolchain

cd micropython-lorawan
make -C mpy-cross

cd ports/esp32
make submodules
make BOARD=LILYGO_TTGO_TBEAM \
     USER_C_MODULES=$(pwd)/../../lorawan-module/micropython.cmake
```

---

## API reference

### `lorawan.LoRaWAN(region, **kwargs)`

Creates and initialises the LoRaWAN stack. Only one instance may exist at a time; reset the board to create a new one.

```python
lw = lorawan.LoRaWAN(
    region=lorawan.EU868,           # required — region constant (see below)

    # --- LoRaWAN protocol ---
    lorawan_version=lorawan.V1_0_4, # V1_0_4 (default) or V1_1
    device_class=lorawan.CLASS_A,   # CLASS_A (default), CLASS_B, CLASS_C

    # --- RX2 window ---
    # TTN EU868 uses DR_3 (SF9/125kHz). Standard LoRaWAN uses DR_0 (SF12).
    rx2_dr=lorawan.DR_3,            # default: DR_3 (TTN EU868)
    rx2_freq=869525000,             # default: 869.525 MHz (Hz)

    # --- TX power ---
    # In dBm EIRP. None = region maximum (16 dBm for EU868).
    # Values within the regulatory limit go through the MAC stack.
    # Values above the limit activate a hardware override (user responsibility).
    #   SX1276 hardware cap: 20 dBm
    #   SX1262 hardware cap: 22 dBm
    tx_power=None,

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

#### `lw.send(data, *, port=1, confirmed=False, datarate=lorawan.DR_0)`

Transmits an uplink frame. Blocks until TX is complete (includes waiting for RX1/RX2 windows — up to 10 s). Raises `RuntimeError` on failure.

```python
lw.send(b"hello")                              # unconfirmed, port 1, DR_0 (SF12)
lw.send(b"ping", confirmed=True)               # confirmed uplink — waits for ACK
lw.send(b"data", port=2, datarate=lorawan.DR_5) # SF7 — faster, shorter range
```

#### `lw.on_tx_done(callback)` / `lw.on_tx_done(None)`

Registers a callback fired after each uplink cycle. For confirmed uplinks, `success=True` means the network ACKed. For unconfirmed, `success=True` means the frame was transmitted.

```python
lw.on_tx_done(lambda ok: print("tx done, ack:", ok))
lw.on_tx_done(None)  # deregister
```

---

### Downlink

#### `lw.recv(*, timeout=10)` → `(bytes, int, int, int) | None`

Blocks up to `timeout` seconds for a downlink. Returns `(data, port, rssi, snr)` or `None`. Use `timeout=0` to poll without blocking.

```python
pkt = lw.recv(timeout=10)
if pkt:
    data, port, rssi, snr = pkt
    print(f"rx port={port} rssi={rssi} snr={snr}: {data}")
```

#### `lw.on_rx(callback)` / `lw.on_rx(None)`

Registers a callback fired on each downlink. Do not combine with `recv()` on the same object.

```python
lw.on_rx(lambda pkt: print("rx:", pkt))  # pkt = (data, port, rssi, snr)
lw.on_rx(None)  # deregister
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

| dBm EIRP | MAC index | SF equivalent |
|----------|-----------|---------------|
| 16 | 0 (max) | any |
| 14 | 1 | |
| 12 | 2 | |
| 10 | 3 | |
| 8 | 4 | |
| 6 | 5 | |
| 4 | 6 | |
| 2 | 7 (min) | |

The setter rounds down to the nearest available step (never exceeds the requested value).

#### `lw.stats()` → `dict`

Returns a snapshot of runtime statistics:

```python
lw.stats()
# {
#   'rssi': -109,        # last downlink RSSI (dBm)
#   'snr': 6,            # last downlink SNR (dB)
#   'tx_counter': 4,     # uplink frame counter
#   'rx_counter': 1,     # downlink frame counter
#   'tx_time_on_air': 991, # last uplink time on air (ms)
#   'last_tx_ack': True,   # confirmed uplink: True if ACKed
# }
```

---

### Persistence

The MAC session (DevAddr, session keys, frame counters, ADR state) can be stored in ESP32 NVS so the device resumes after a reboot without re-joining.

**Critical boot sequence:**

```python
lw = lorawan.LoRaWAN(region=lorawan.EU868)

try:
    lw.nvram_restore()  # must be called before join on every boot
except OSError:
    pass  # first boot — no saved session yet

if not lw.joined:
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

### Module-level

#### `lorawan.version()` → `str`

Returns the module version string, e.g. `'0.6.0'`.

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
lorawan.CLASS_B  # Class B — scheduled ping slots (Phase 5)
lorawan.CLASS_C  # Class C — continuous RX2 (Phase 5)

# Data rates (EU868): higher index = higher SF = longer range, slower
lorawan.DR_0   # SF12 / 125 kHz — maximum range
lorawan.DR_1   # SF11 / 125 kHz
lorawan.DR_2   # SF10 / 125 kHz
lorawan.DR_3   # SF9  / 125 kHz
lorawan.DR_4   # SF8  / 125 kHz
lorawan.DR_5   # SF7  / 125 kHz — maximum throughput
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

hw.radio    # 'sx1276' or 'sx1262'
hw.pmu      # 'axp192', 'axp2101', or None (v0.7)
hw.irq_pin  # radio IRQ GPIO
hw.busy_pin # SX1262 BUSY GPIO, or None for SX1276
hw.gps_rx   # GPS UART RX pin (ESP32 side)
hw.gps_tx   # GPS UART TX pin (ESP32 side)
hw.has_oled # True if SSD1306 found on I2C
```

`detect()` probes SPI, I2C, and optionally the GPS UART. Cache the result if calling multiple times — it takes 2–4 s on the first call.

### Raw LoRa TX/RX

```python
import tbeam

hw  = tbeam.detect()
lm  = tbeam.lora_modem(hw)   # SX1276 or SX1262 SyncModem

# Configure the radio (all fields optional — only set what changes)
lm.configure({
    "freq_khz":    868100,  # frequency in kHz
    "sf":          7,       # spreading factor 7–12
    "bw":          "125",   # bandwidth in kHz: "125", "250", "500"
    "coding_rate": 5,       # 5=4/5, 6=4/6, 7=4/7, 8=4/8
    "output_power": 14,     # TX power in dBm (SX1276 PA_BOOST: 2–20; SX1262: 2–22)
    "preamble_len": 8,      # preamble symbols
    "crc_en":      True,    # CRC on packet
    "implicit_header": False,
})

# Transmit
lm.send(b"hello world")

# Receive — blocks up to timeout_ms milliseconds, returns bytes or None
pkt = lm.recv(timeout_ms=5000)
if pkt:
    print("rx:", pkt, "rssi:", lm.last_rssi)
```

`lora_modem()` handles SX1262-specific initialisation automatically (DIO3 TCXO at 1.8V, DIO2 RF switch).

### Other `tbeam` helpers

```python
# GPS UART (NEO-6M / NEO-M8N)
uart = tbeam.gps_uart(hw)          # returns machine.UART, baudrate=9600
line = uart.readline()

# I2C bus (PMU, OLED, sensors)
i2c = tbeam.i2c_bus()              # returns machine.I2C at 400 kHz

# Low-level SPI + pin access (for custom driver use)
spi  = tbeam.lora_spi(baudrate=10_000_000)
pins = tbeam.lora_pins(hw)         # (cs, irq, rst) or (cs, irq, rst, busy)
```

---

## Examples

### OTAA sensor node (TTN, standard setup)

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868)

try:
    lw.nvram_restore()
except OSError:
    pass

if not lw.joined:
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

lw = lorawan.LoRaWAN(region=lorawan.EU868)

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

### Standard LoRaWAN network (non-TTN, RX2 DR_0)

```python
import lorawan

# TTN uses RX2 DR_3 (SF9). Standard LoRaWAN EU868 uses DR_0 (SF12).
lw = lorawan.LoRaWAN(region=lorawan.EU868, rx2_dr=lorawan.DR_0)
```

### LoRaWAN 1.1 OTAA

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868, lorawan_version=lorawan.V1_1)
lw.join_otaa(
    dev_eui=bytes.fromhex("70B3D57ED0000000"),
    join_eui=bytes.fromhex("0000000000000000"),
    app_key=bytes.fromhex("00000000000000000000000000000000"),
    nwk_key=bytes.fromhex("11111111111111111111111111111111"),
)
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

### Callbacks (async pattern)

```python
import lorawan

lw = lorawan.LoRaWAN(region=lorawan.EU868)
# ... join ...

lw.on_rx(lambda pkt: print("downlink:", pkt))
lw.on_tx_done(lambda ok: print("tx ack:", ok))

lw.send(b"ping", confirmed=True)
```

---

## Hardware notes (confirmed on device)

**SX1262 TCXO (DIO3, 1.8V).** The T-Beam SX1262 uses an internal TCXO via DIO3. The HAL calls `SX126xSetDio3AsTcxoCtrl(TCXO_CTRL_1_8V)` as the first operation in `SX126xIoInit()`. Without it: `XOSC_START_ERR (0x20)`.

**SX1262 DIO2 as RF switch.** DIO2 controls the TX/RX antenna switch. Enabled via `SX126xSetDio2AsRfSwitchCtrl(true)`.

**SX1276 uses a crystal, not TCXO.** `REG_LR_TCXO = 0x09`. Setting 0x19 (TCXO mode) silences the radio.

**SPI bus exclusivity.** The HAL owns the SPI bus exclusively. Do not create a `machine.SPI` instance on the same bus while the LoRaWAN stack is running.

**TX power hardware maximums.** The SX1262 outputs up to +22 dBm; the SX1276 (PA_BOOST) up to +20 dBm. Both are below the EU868 regulatory EIRP limit of 16 dBm. Going above 16 dBm requires a hardware TX power override (see `tx_power()` above) and is the user's responsibility. Reaching +30 dBm requires an external PA (e.g. Ebyte E22-900M30S module); the software hook point is `SX126xAntSwOn()`/`SX126xAntSwOff()` in `sx126x_board.c`.

**RX2 default.** TTN EU868 uses 869.525 MHz / DR_3 (SF9). Standard LoRaWAN specifies DR_0 (SF12) on the same frequency. Pass `rx2_dr=lorawan.DR_0` for non-TTN networks.

---

## Project status

v0.6.0. Phase 4 complete (ABP, OTAA, uplink/downlink, confirmed messages, ADR, NVS persistence). Phase 5 in progress: runtime pin config, LoRaWAN version selection, configurable RX2 and TX power. Class C, Device Time, Class B beacons, and multicast planned for Sessions 12–14.

See [TODO.md](TODO.md) for the development roadmap and [CLAUDE.md](../CLAUDE.md) for project context including hardware constants and FreeRTOS pitfalls.

## Architecture

```
lorawan-module/
├── micropython.cmake       build entry point (INTERFACE library)
├── bindings/
│   └── modlorawan.c        Python bindings + FreeRTOS task
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
│   └── pin_config.c        runtime pin config + TX power override
├── config/
│   └── lorawan_config.h    T-Beam defaults, region flags
└── loramac-node/           Semtech LoRaMAC-node v4.7.0 (copied sources)
    ├── src/mac/            LoRaMac.c, regions
    ├── src/radio/          SX1276, SX126x drivers
    └── src/peripherals/    soft-se (AES, CMAC)
```

The LoRaWAN MAC runs in a dedicated FreeRTOS task on CPU1 (priority 6, 8 KB stack). Python communicates via a command queue and event group. All `mp_printf` / MicroPython calls are confined to `mp_sched_schedule` trampolines running on the Python thread.

## Related repositories

| Repository | Purpose |
|---|---|
| [LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0) | Semtech's LoRaWAN MAC stack |
| [MicroPython](https://github.com/micropython/micropython) | Upstream |

## License

MicroPython is licensed under the MIT license. LoRaMAC-node is licensed under the Revised BSD License. See [LICENSE](LICENSE) for details.
