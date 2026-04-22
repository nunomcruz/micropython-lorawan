MicroPython + LoRaWAN (T-Beam)
==============================

Fork of [MicroPython](https://micropython.org/) v1.29.0-preview with a full LoRaWAN MAC stack for TTGO T-Beam boards.

## What this fork adds

A `USER_C_MODULE` that wraps [Semtech LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0), providing OTAA/ABP join, uplink, downlink, confirmed messages, ADR, and NVS session persistence. The module is accessed from Python as `import lorawan`.

The target hardware is the LILYGO TTGO T-Beam family, with runtime auto-detection of all known variants (v0.7 through v1.2, SX1276 and SX1262 radios, AXP192 and AXP2101 PMUs). A single firmware image supports all variants — no need for separate builds.

The primary target region is EU868 (TTN), with EU433 and other regions planned.

## Supported hardware

| T-Beam version | Radio | PMU | GPS |
|----------------|-------|-----|-----|
| v0.7 | SX1276 | None (TP4054) | NEO-6M |
| v1.0 | SX1276 | AXP192 | NEO-6M |
| v1.1 | SX1276 / SX1262 | AXP192 | NEO-6M / NEO-M8N |
| v1.2 | SX1276 / SX1262 | AXP2101 | NEO-M8N |

## Quick start

```bash
# Build mpy-cross
make -C mpy-cross

# Build for T-Beam (once lorawan-module/ is ready)
cd ports/esp32
make submodules
make BOARD=LILYGO_TTGO_TBEAM \
     USER_C_MODULES=$(pwd)/../../lorawan-module/micropython.cmake
```

## Usage example (ABP — confirmed working)

```python
import lorawan

# Init: detects radio (SX1276 reg 0x42 = 0x12), starts LoRaWAN task on CPU1
lw = lorawan.LoRaWAN(region=lorawan.EU868)

# ABP join (LoRaWAN 1.0 keys)
lw.join_abp(
    dev_addr=0x260B0000,
    nwk_s_key=bytes.fromhex("00000000000000000000000000000000"),
    app_s_key=bytes.fromhex("00000000000000000000000000000000"),
)

# Send uplink — default DR_0 (SF12) for maximum range
lw.send(b"hello", port=1)
lw.send(b"hello", port=1, datarate=lorawan.DR_5)  # SF7 if closer to gateway

# Data rate constants: DR_0=SF12 (best range) .. DR_5=SF7 (fastest)
# Device class constants: CLASS_A, CLASS_B, CLASS_C
```

OTAA join (`join_otaa`) is planned for Session 8.

## Hardware findings (confirmed on device)

Discovered during Phase 1 testing on a T-Beam v1.1 SX1262/AXP192. Relevant for the Phase 3 C HAL.

**SX1262 TCXO (DIO3, 1.8V).** The T-Beam SX1262 uses an internal TCXO powered via DIO3. The driver must call `SetDIO3AsTCXOCtrl` at 1.8V before any calibration or RF operation. Without it the radio raises `XOSC_START_ERR (0x20)` on first use. In Python: `dio3_tcxo_millivolts=1800`. In C HAL: `SX126xSetDio3AsTcxoCtrl(TCXO_CTRL_1_8V, ...)` as the first call in `SX126xIoInit()`.

**SX1262 DIO2 as RF switch.** DIO2 controls the TX/RX antenna switch. Must enable with `SX126xSetDio2AsRfSwitchCtrl(true)`.

**SPI bus ownership.** The SPI bus must be owned exclusively by one driver. Mixing a Python `machine.SPI(1)` object with a second driver instance on the same bus causes RC13M + ADC calibration failures (`OpError 0xa00`). The `tbeam.lora_modem()` helper is the correct single entry point — do not create a `SPI(1)` manually before calling it.

**SX1276 uses a crystal, not TCXO.** `REG_LR_TCXO = 0x09` (crystal mode). Setting 0x19 (TCXO mode) silences the radio.

## Project status

Phase 4 Session 7 complete: ABP join and uplink confirmed working on T-Beam v1.1 SX1276/AXP192. MAC initialisation, ABP join, and TX confirmed at the MAC layer. Sessions 8–10 (OTAA, downlink, persistence) in progress.

See [TODO.md](TODO.md) for the development roadmap and [CLAUDE.md](../CLAUDE.md) for project context including hardware constants and FreeRTOS pitfalls.

## Architecture

The LoRaWAN module lives in `lorawan-module/` (inside this repo) and is compiled via `USER_C_MODULES`. It includes:

- ESP32 HAL layer (GPIO, SPI, Timer, Delay) using ESP-IDF APIs
- LoRaMAC-node v4.7.0 MAC stack (copied, not a submodule)
- Radio board HAL for both SX1276 and SX126x
- Python bindings (`modlorawan.c`)

A frozen `tbeam.py` module handles hardware auto-detection at the Python level.

## Related repositories

| Repository | Purpose |
|---|---|
| [LoRaMAC-node v4.7.0](https://github.com/Lora-net/LoRaMac-node/tree/v4.7.0) | Semtech's LoRaWAN stack (source for the C module) |
| [MicroPython](https://github.com/micropython/micropython) | Upstream MicroPython |

## Upstream MicroPython

This fork is based on MicroPython v1.29.0-preview. For general MicroPython documentation, see [docs.micropython.org](https://docs.micropython.org/). For information about the ESP32 port, see the [ESP32 quick reference](https://docs.micropython.org/en/latest/esp32/quickref.html).

## License

MicroPython is licensed under the MIT license. LoRaMAC-node is licensed under the Revised BSD License. See [LICENSE](LICENSE) for details.
