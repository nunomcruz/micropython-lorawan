# Changelog

Development history of the LoRaWAN MAC stack on top of MicroPython v1.29.0-preview, targeting LILYGO TTGO T-Beam variants. Entries are listed newest-first. The Python API has been stable since **v1.0.0**; only additive minor-version changes have been made since.

## v1.4.0 — Additional regions

- Build-time region opt-in. `LORAWAN_REGIONS` cmake cache variable selects which regions to compile in (default `EU868`). Multi-region build example: `-DLORAWAN_REGIONS="EU868;EU433;US915;AS923"`.
- `RegionBaseUS.c` is added automatically when US915 or AU915 is enabled (shared 72-channel layer).
- Region-aware `tx_power()` table — `dbm_to_tx_power` / `tx_power_to_dbm` now dispatch through a per-region EIRP table sourced from each region header. **Note:** EU433 users on previous versions saw EU868 dBm values; from v1.4.0 they correctly get the EU433 maximum (12 dBm).
- All 10 region constants (`EU868`, `EU433`, `US915`, `AU915`, `AS923`, `KR920`, `IN865`, `RU864`, `CN470`, `CN779`) are always exported. Passing an uncompiled region raises `OSError` at construction. (MicroPython's QSTR scanner does not propagate `target_compile_definitions` from user modules, so `#ifdef` guards on QSTR-bearing constants would silently break the build.)
- Firmware: EU868-only 0x192ff0 bytes; EU868+EU433+US915+AS923 ≈ 0x1959f0 bytes (~3.2 KB per extra region).

## v1.3.1 — Hardware bug fixes

- `derive_mc_keys()` returned all zeros and could leak across groups — fixed.
- `tx_cw()` hardware failures fixed.

## v1.3.0 — New MAC primitives

- `lw.tx_cw(freq_hz, power_dbm, duration_s)` — wraps `MLME_TXCW`. Continuous-wave transmission for radio bring-up, antenna SWR check, FCC/CE pre-compliance scans. Validates frequency against region bounds, power against hardware caps (SX1276 ≤ 20 dBm; SX1262 ≤ 22 dBm), duration 1–30 s.
- `lw.compliance_enable()` registers `LmhpCompliance` (port 224). Required by LoRa Alliance certification testing — the package responds to DUT commands from the certification tool autonomously.
- `lw.derive_mc_keys(addr, mc_root_key, *, group=0)` — performs in-MAC multicast session-key derivation. Replaces caller-side AES that previously had to be plumbed manually before `multicast_add()`.

## v1.1.0 — Error handling and diagnostics

- Every failure path now raises a typed exception that carries the underlying LoRaMAC status code. ~12 generic `RuntimeError("<api> failed")` sites collapsed into `OSError(EIO, "<api>: status=N")` carrying the `LoRaMacStatus_t` or `LoRaMacEventInfoStatus_t`.
- `lw.last_error()` returns a diagnostic dict `{"loramac_status", "event_status", "context", "epoch_us"}` for post-mortem after any `OSError(EIO)`.
- OTAA join timeout now raises `OSError(ETIMEDOUT, "join: last_status=N")` with the captured event status.
- `EBUSY` semantics confirmed: `EVT_TX_DUTY_CYCLE` is only set when `LORAMAC_STATUS_DUTYCYCLE_RESTRICTED` (frame **not** queued); the `OK` path with non-zero `DutyCycleWaitTime` queues internally and eventually fires `mcps_confirm`.
- Constructor auto-deinit no longer dereferences a potentially dangling `s_lora_obj`.

## v1.0.0 — Stable API surface

API refinement before declaring the surface stable.

- Constructor kwarg `rx2_dr` renamed to `rx2_datarate` for consistency with the `datarate()` method.
- `request_class()` deprecated in favour of `device_class(cls)` overload — Class A/C transitions are synchronous; Class B is async and notified via `on_class_change`.
- All callbacks unified to receive separate positional args (no tuples): `on_recv(data, port, rssi, snr, multicast)`, `on_beacon(state, info)`, `on_progress(counter, nb, size, lost)`, `on_done(status, size)`.
- `recv()` returns the same 5-tuple shape as `on_recv`.
- Exception types standardised: `OSError(errno)` for I/O and timeout, `ValueError` for invalid arguments, `RuntimeError` for state preconditions only (e.g. *not joined*, *package not enabled*).
- Documented `recv()` ↔ `on_recv()` interaction (recv has priority while VM is blocked; on_recv silenced).

## v0.18.0 — Battery-level reporting

- `lw.battery_level([level])` — getter/setter exposed for the byte sent in `DevStatusAns` (LoRaWAN spec semantics: `0` = USB powered, `1..254` = relative charge, `255` = unknown). Volatile single-byte storage in the HAL — atomic on xtensa, no mutex needed.
- `examples/sensor_node.py` reads the AXP192 battery voltage, maps it to the LoRaWAN range and calls `battery_level()` before each `send()`.

## v0.17.0 — DR control on MAC-command uplinks

Two UX gaps surfaced during EU868 cell-edge field tests.

- `clock_sync_request(*, datarate=None)` — optional DR override for the AppTimeReq (port 202). When ADR has ratcheted the DR up to SF7, range-limited port-202 frames are silently dropped at the gateway; passing `DR_0` for the sync only is now possible without leaking into subsequent user uplinks.
- `link_check(*, send_now=False, datarate=None, port=1, confirmed=False)` — opt-in explicit uplink. Default behaviour (piggy-back on next `send()`) is preserved; `send_now=True` emits a probe frame and returns the fresh `{margin, gw_count}` in one call. `port` and `confirmed` kwargs added.

## v0.16.0 — Teardown ordering hardening

Follow-up to v0.12.0's lifecycle work.

- DIO ISRs are now deregistered **before** `LoRaMacDeInitialization()` so an in-flight interrupt cannot dispatch into half-torn-down MAC state. New `SX1276IoIrqDeInit()` / `SX126xIoIrqDeInit()` HAL entry points.
- Bounded-wait blindness fixed: if the LoRaWAN task does not acknowledge `CMD_DEINIT` within 2 s, the Python side logs a warning and **does not** delete the FreeRTOS primitives — a small leak is strictly better than a use-after-free.
- Considered and rejected `eTaskGetState` polling — its FreeRTOS implementation dereferences the handle directly and would UAF as soon as the idle task on CPU1 frees the TCB.

## v0.15.0 — Stats expansion

- `stats()` extended with `last_tx_dr`, `last_tx_freq`, `last_tx_power` (dBm EIRP). Captured in `mcps_confirm` from `Datarate / Channel / TxPower`. Distinct from the live `datarate()` / `tx_power()` getters, which reflect the *next* TX values after any ADR adjustment.
- `last_tx_power` converted to dBm via the per-region EIRP table for consistency with `tx_power()`.

## v0.14.0 — Missing getters

- `lw.deinit()` — ordered teardown dispatched through `CMD_DEINIT`. Stops the MAC, drops package registrations + FUOTA buffer, releases the SPI bus, deletes the shared `esp_timer`, then `vTaskDelete(NULL)`. Idempotent.
- Soft-reset hook via `mp_obj_malloc_with_finaliser` + `__del__ → deinit`. `gc_sweep_all()` runs during the ESP32 port's soft-reset and fires the finaliser automatically — `Ctrl-D` is now safe.
- `__enter__` / `__exit__` for `with lorawan.LoRaWAN(...) as lw:` cleanup on scope exit.
- `lw.dev_addr()` — current 32-bit DevAddr from `MIB_DEV_ADDR`.
- `lw.region()` — returns the `LORAMAC_REGION_*` value cached at `__init__`.
- `lw.max_payload_len()` — wraps `LoRaMacQueryTxPossible` so callers can slice payloads before `send()` instead of catching a late `LENGTH_ERROR`.

## v0.13.0 — Duty-cycle visibility

Confirmed: duty cycle IS enforced (`EU868_DUTY_CYCLE_ENABLED = 1`). Back-to-back `send()` calls only *appeared* to ignore it because the previous hard-coded 10 s wait timed out while the MAC quietly held the frame for `TxDelayedTimer` to flush.

- `send(..., timeout=...)` — default 120 s (covers a duty-cycled DR_0 uplink); `timeout=None` blocks until TX completes; non-positive values map to *wait forever*.
- `lw.duty_cycle([enabled])` — getter from `Nvm.MacGroup2.DutyCycleOn`; setter dispatches `LoRaMacTestSetDutyCycleOn(bool)` (the only public flip in v4.7.0). Disabling logs a non-conformance warning.
- `lw.time_until_tx()` — milliseconds until next TX is allowed, from `DutyCycleWaitTime` captured at the most recent `send()`. Returns 0 before the first send, when DC is off, or once the wait window has elapsed.

## v0.12.0 — Lifecycle (foundation)

Foundation for `deinit()` / soft-reset safety / context manager. Subsequent versions hardened the teardown ordering and the bounded-wait paths.

## v0.11.0 — Multicast and FUOTA

- Local multicast — `multicast_add(group, addr, mc_nwk_s_key, mc_app_s_key, ...)` (up to 4 groups), `multicast_rx_params(group, device_class, frequency, datarate, ...)` for Class B (with periodicity) or Class C, `multicast_remove(group)`, `multicast_list()`.
- Remote multicast — `remote_multicast_enable()` registers `LmhpRemoteMcastSetup` (port 200). The server can then provision groups via `McGroupSetupReq` / `McGroupClassCSessionReq` / `McGroupClassBSessionReq` with no further Python glue. The package drives the class switch automatically via the shim's `LmHandlerRequestClass`.
- Fragmentation / FUOTA — `LmhpFragmentation` (port 201) registered; `fragmentation_enable(buffer_size, on_progress=, on_done=)`. The shim owns the `FragDecoder` memory-backed read/write callbacks (flat RAM buffer outside the MicroPython GC heap). `fragmentation_data()` returns the reassembled buffer as `bytes` after `on_done` has fired.
- `on_recv` callback gained a fifth `multicast` flag.

## v0.10.0 — Class B (beacons + ping slots)

- `LORAMAC_CLASSB_ENABLED` enabled — `LoRaMacClassB.c` compiled in.
- `BoardGetTemperatureLevel()` returns a fixed 25.0 °C estimate, used by `ApplyTemperatureDrift` for crystal-drift compensation on beacon timing (matches the Nucleo reference port pattern when no sensor is present).
- `request_class(CLASS_B)` is async and multi-step: `MLME_BEACON_ACQUISITION` → `MLME_PING_SLOT_INFO` → `MIB_DEVICE_CLASS = CLASS_B`. Beacon-acquisition failure transparently re-syncs via a fresh `MLME_DEVICE_TIME` + retry.
- `ping_slot_periodicity(N)` (period `2^N` seconds, applied on next `device_class(CLASS_B)`).
- `on_beacon(callback)` fires for `BEACON_ACQUISITION_OK`, `BEACON_ACQUISITION_FAIL`, `BEACON_LOCKED`, `BEACON_NOT_FOUND`, `BEACON_LOST`.
- `beacon_state()` returns the most recent beacon dict (state, GPS time, freq, DR, RSSI, SNR, gw_info).
- `MLME_BEACON_LOST` is the real *2 hours without beacon* revert path: `MIB_DEVICE_CLASS = CLASS_A`, all flags cleared, callbacks fire.

## v0.9.3 — Clock Sync package

- `bindings/lmhandler_shim.c` — minimal LmHandler adapter that supplies the three functions `LmhpClockSync.c` calls (`LmHandlerIsBusy`, `LmHandlerSend`, `LmHandlerPackageRegister`). The full `LmHandler.c` is **not** linked: it carries its own `LoRaMacInitialization` path and would collide with our task design.
- `clock_sync_enable()`, `clock_sync_request()` and `synced_time()` — the `OnSysTimeUpdate` hook updates `time_synced` and `network_time_gps` from both the AppTimeAns path and the MAC-level DeviceTimeAns path; `on_time_sync(cb)` fires for both with identical semantics.

## v0.9.2 — ReJoin (LoRaWAN 1.1)

- `lw.rejoin(type=0|1|2)` — wraps `MLME_REJOIN_0/1/2`. Unlike `link_check()` / `request_device_time()` (piggy-back), a rejoin is itself an uplink — no follow-up `send()` is needed.

## v0.9.1 — `on_time_sync` notifications

- `on_time_sync(callback)` — schedules a Python trampoline that allocates the GPS-epoch int in VM context. Necessary because the GPS epoch in 2026 (≈1.4 B) exceeds the 31-bit `MP_OBJ_NEW_SMALL_INT` range used by the scheduler arg.

## v0.9.0 — DeviceTimeReq

- `request_device_time()` — queues an MLME `DEVICE_TIME` MAC command; piggy-backs on the next uplink.
- `network_time()` — captures the GPS epoch from `mlme_confirm(MLME_DEVICE_TIME)`. (Removed in v1.0.0 — superseded by `synced_time()` and the snapshot arg in `on_time_sync`.)
- `link_check()` — caches `DemodMargin` and `NbGateways` from `mlme_confirm(MLME_LINK_CHECK)`; piggy-backs on the next uplink.

## v0.8.0 — Antenna gain and EU433

- `antenna_gain` constructor kwarg + `antenna_gain()` getter/setter. Default 0.0 so the radio emits the full EIRP — the EU868 region default of 2.15 dBi was under-driving the radio by 2.15 dB versus the requested EIRP.
- EU433 region support compiled in (`REGION_EU433`, `RegionEU433.c`). RX2 default simplified: `__init__` no longer forces any RX2 override; without `rx2_datarate` / `rx2_freq` kwargs the MAC uses the region's `PHY_DEF_RX2`.

## v0.7.0 — Class C

- `request_class(CLASS_A|CLASS_C)`, `device_class()`, `on_class_change(cb)`. Class A ↔ C is synchronous via `MIB_DEVICE_CLASS` from task context.
- **RxC window fix** — LoRaMAC-node v4.7.0 has a separate `RxCChannel` alongside `Rx2Channel`; `MIB_RX2_CHANNEL` only writes the latter. `OpenContinuousRxCWindow()` reads `RxCChannel.Datarate` (default DR_0/SF12 in EU868), so without an explicit `MIB_RXC_CHANNEL` write the radio listens at SF12 while TTN transmits at DR_3/SF9. `CMD_INIT` now writes both pairs.

## v0.6.0 — Runtime configuration

- Full runtime pin configuration from Python — `radio="sx1276"|"sx1262"`, `spi_id`, `mosi`, `miso`, `sclk`, `cs`, `reset`, `irq`, `busy` constructor kwargs. T-Beam values are still the default; non-T-Beam boards just override the pins they need.
- LoRaWAN version selection — `lorawan_version=lorawan.V1_0_4|lorawan.V1_1`. ABP path sets `MIB_ABP_LORAWAN_VERSION`; OTAA gains an optional `nwk_key=` kwarg for 1.1.
- Configurable RX2 — `rx2_datarate`, `rx2_freq` constructor kwargs.
- TX power in dBm EIRP — `tx_power=14` constructor kwarg + `tx_power()` getter/setter. EU868 step is 2 dBm, range 2–16 dBm. The setter rounds down (never exceeds the requested EIRP).
- Hardware TX-power override — `lw.tx_power(20)` activates a hardware override beyond the regulatory limit (user responsibility). SX1276 ≤ 20 dBm, SX1262 ≤ 22 dBm. Override and ADR are mutually exclusive: setting the override auto-disables ADR; enabling ADR clears the override.

### SX1262 sleep/wakeup bug

End-to-end SX1262 testing surfaced the sleep/wakeup race. After RX1/RX2 windows, `RadioSleep(WarmStart=1)` put the radio to sleep with BUSY held LOW; the next command's `WaitOnBusy()` returned immediately and clocked data into a half-awake radio.

Fix: `SX126xWakeup()` now sets `operating_mode = MODE_STDBY_RC` after waking the radio, so wakeup only triggers once per sleep cycle. `ensure_awake()` is called inside `spi_lock()` before `WaitOnBusy()` in all six SPI HAL functions. The SPI mutex was changed from `xSemaphoreCreateMutex` to `xSemaphoreCreateRecursiveMutex` so that `SX126xWakeup()` (which itself does SPI) can run while holding the lock.

## v0.5.0 and earlier — Phase 4 foundations

The full Python binding surface taking shape across Sessions 7–10:

- ABP join + first uplink — `lorawan_obj_t`, `LoRaWAN.__init__`, dedicated FreeRTOS task on CPU1 (priority 6, 8 KB stack), command queue + event group, EU868 RX2 = 869.525 MHz / DR_3 for TTN, LoRaWAN 1.0.4 single-key MIC for ABP. Three FreeRTOS pitfalls fixed during this work: GIL deadlock from `mp_printf` in non-Python tasks, ESP-IDF v5 stack depth in bytes (not words), `pdMS_TO_TICKS(2) = 0` busy-spin starving `xKernelLock`.
- OTAA join — full procedure with retry loop, `joined()`, `stats()`.
- Receive + confirmed messages — `recv(timeout=...)`, `on_recv(callback)`, `on_send_done(callback)`, `last_tx_ack`.
- Persistence + ADR — `nvram_save()`, `nvram_restore()`, `datarate()`, `adr()`, `tx_power()` (MAC index). DevNonce auto-save after `MLME_JOIN OK`. Explicit CRC recomputation in `nvram_save` to avoid `RestoreNvmData` silently skipping the Crypto group.

## Phase 3 and earlier — HAL and project skeleton

- ESP32 HAL: GPIO, SPI (`SPI3_HOST`, 10 MHz), timers (one `esp_timer` + sorted linked list), delay, board init, SX1276 and SX1262 radio HAL, runtime radio selection via separate `Radio_SX1276` / `Radio_SX126x` tables and a writable `Radio` global.
- Dual-radio symbol isolation — `hal/sx1276_radio_wrapper.c` and `hal/sx126x_radio_wrapper.c` rename `TxTimeoutTimer` / `RxTimeoutTimer` / `FskBandwidths` / `Radio` via `#define` before `#including` originals, so `loramac-node/` sources stay unmodified.
- USER_C_MODULE skeleton — `lorawan-module/` directory layout, `micropython.cmake` INTERFACE library, vendored LoRaMAC-node v4.7.0 sources.
- T-Beam board definition — `ports/esp32/boards/LILYGO_TTGO_TBEAM/` (all variants, runtime auto-detect via `tbeam.py`), frozen `lora-sx127x` and `lora-sx126x` drivers.

## Roadmap

Items considered but not yet implemented. None are blockers; each is an additive minor-version candidate.

- **Expanded MIB getters/setters** — `nb_trans()` (per-uplink retransmission count, cheapest reliability knob), `channel_mask()`, `frame_counters()` (live `(fcnt_up, fcnt_down)` from `MIB_NVM_CTXS`), `public_network()` (sync word selection for private deployments), `rx_window_timing()`, `rx_clock_drift()` (useful after long deepsleep with RTC drift), `rejoin_cycle()`, `net_id()`. Each is a thin wrapper on existing MAC state.
- **`__doc__` strings** on Python-facing methods, if MicroPython v1.29 supports it on `MP_DEFINE_CONST_FUN_OBJ`.

## Hardware notes (not version-specific)

- TCXO register on **SX1276**: T-Beam uses a crystal (`REG_LR_TCXO = 0x09`), NOT TCXO (`0x19`). Wrong value silences the radio.
- EU868 RX2 DR: TTN expects DR_3 (SF9). The LoRaWAN spec default is DR_0 (SF12).
- LoRa Reset: GPIO 23 across all T-Beam versions.
- GPS pins differ: v0.7 uses RX=12 / TX=15; v1.0+ uses RX=34 / TX=12.
- v0.7 has no PMU — battery is monitored via the TP4054 charger and an ADC voltage divider.
