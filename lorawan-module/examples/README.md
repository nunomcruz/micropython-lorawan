# LoRaWAN examples

Runnable scripts for the `lorawan` USER_C_MODULE. All examples assume a T-Beam
board and use `tbeam.detect()` for hardware introspection.

Copy one to the board as `main.py` (or run it from the REPL with
`exec(open("basic_otaa.py").read())`). Before running, edit the credentials at
the top of the script with the values from your LNS console.

| Script | Purpose |
|---|---|
| [basic_otaa.py](basic_otaa.py) | OTAA join + one unconfirmed uplink. |
| [basic_abp.py](basic_abp.py) | ABP session + one confirmed uplink. |
| [sensor_node.py](sensor_node.py) | Periodic sensor uplink with deep sleep, AXP192 battery read, and `battery_level()` reporting for `DevStatusAns`. |
| [time_sync.py](time_sync.py) | Sync to network time via MAC DeviceTimeReq and the Clock Sync package. |
| [class_b_beacon.py](class_b_beacon.py) | Switch to Class B, monitor beacons, receive on ping slots. |
| [multicast_receiver.py](multicast_receiver.py) | Provision a local multicast group and listen in Class C. |
| [test_session24_primitives.py](test_session24_primitives.py) | Regression tests for v1.3.0 primitives: `tx_cw()`, `compliance_enable()`, `derive_mc_keys()`. |

## Credentials

The examples ship with placeholder credentials (all zeros). They will fail to
join any real network until you replace:

- `DEV_EUI` — per-device identifier, 8 bytes big-endian.
- `JOIN_EUI` — application identifier, 8 bytes big-endian (often all zeros for
  private networks).
- `APP_KEY` — root key, 16 bytes.

TTN displays all three in the *End device overview* page in the format
required here (MSB / big-endian hex).

## NVS session

Most examples call `nvram_restore()` on boot and `nvram_save()` after the
uplink. This is not optional — without it TTN will reject every subsequent
join with `devnonce is too small`, and frame counters reset to 0 on each
reboot (causing duplicate-counter rejections).

If you need to re-run OTAA from scratch (e.g. after resetting the device on
the LNS console), erase the `lorawan` NVS namespace:

```python
import esp32
esp32.NVS("lorawan").erase_key("nvm_ctx")  # namespace + blob name used by the module
```

## Prerequisites for some scripts

- `class_b_beacon.py` requires a gateway that advertises the Class B beacon.
  Many community gateways do not — check with your LNS operator.
- `multicast_receiver.py` requires the LNS to support multicast (TTN supports
  it via the Application Server API; ChirpStack has first-class multicast
  groups).
