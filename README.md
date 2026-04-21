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
     USER_C_MODULES=/path/to/lorawan-module/micropython.cmake
```

## Usage example

```python
import tbeam
import lorawan

hw = tbeam.detect()
print(hw)  # HardwareInfo(radio=sx1276, pmu=axp192, ...)

lw = lorawan.LoRaWAN(region=lorawan.EU868)
lw.join_otaa(
    dev_eui=bytes.fromhex("..."),
    join_eui=bytes.fromhex("..."),
    app_key=bytes.fromhex("..."),
    timeout=30,
)

lw.send(b"\x01\x02\x03", port=10)
data, port, rssi, snr = lw.recv(timeout=10)
```

## Project status

This is a work in progress. See [TODO.md](TODO.md) for the development roadmap and [CLAUDE.md](CLAUDE.md) for detailed project context.

## Architecture

The LoRaWAN module lives outside the MicroPython tree in `lorawan-module/` and is compiled via `USER_C_MODULES`. It includes:

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
