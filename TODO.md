# TODO — LoRaWAN for MicroPython

Development roadmap based on MIGRATION_PLAN.md. Each phase maps to one or more Claude Code sessions.

## Phase 0 — Environment Setup (Session 1) ✓

- [x] Verify MicroPython compiles for ESP32_GENERIC (`make -C mpy-cross && cd ports/esp32 && make submodules && make BOARD=ESP32_GENERIC`)
- [x] Verify LILYGO_TTGO_LORA32 board compiles (`make BOARD=LILYGO_TTGO_LORA32`)
- [x] Test USER_C_MODULE example: compiled with `cexample` + `cppexample` (usermod_cexample, usermod_cppexample confirmed by build); flash/REPL test requires hardware
- [x] Confirm loramac-node v4.7.0 is cloned and directory structure understood

## Phase 1 — T-Beam Board Definition (Session 2) ✓

- [x] Create `ports/esp32/boards/LILYGO_TTGO_TBEAM/` using LORA32 as template
- [x] Create `mpconfigboard.h`, `mpconfigboard.cmake`, `board.json`, `board.md`
- [x] Create `sdkconfig.board` with SPIRAM and partition config
- [x] Create `manifest.py` — freeze both lora-sx127x and lora-sx126x drivers plus tbeam.py
- [x] Create `modules/tbeam.py` — HardwareInfo class, _detect_radio(), _detect_pmu(), _detect_gps_pins(), _detect_oled()
- [x] Compile LILYGO_TTGO_TBEAM board (1565KB, 23% of 4MB partition used; all lora drivers + tbeam.py frozen)
- [x] Flash and test `tbeam.detect()` on T-Beam v1.1 SX1262/AXP192: radio=sx1262, pmu=axp192, irq=33, busy=32, gps_rx=34, gps_tx=12
- [x] Test raw LoRa TX via tbeam.lora_modem(): SF7/125kHz/14dBm on 868.1MHz, TX done confirmed

## Phase 2 — USER_C_MODULE Skeleton (Session 3) ✓

- [x] Create `lorawan-module/` directory structure (hal/, bindings/, config/, loramac-node/)
- [x] Copy required LoRaMAC-node sources: src/mac/, src/radio/sx1276/, src/radio/sx126x/, src/radio/radio.h, src/peripherals/soft-se/, src/boards/utilities.h
- [x] Create `micropython.cmake` with INTERFACE library pattern, all source files and include paths
- [x] Create `modlorawan.c` skeleton — version() function, MP_REGISTER_MODULE
- [x] Create `lorawan_config.h` — pin defines, region defines, radio selection
- [x] Create HAL stub files (empty .c with correct headers)
- [x] Compile with USER_C_MODULES and verify `import lorawan; lorawan.version()` works — confirmed on T-Beam hardware, returns `'0.1.0'`

## Phase 3 — ESP32 HAL (Sessions 4–6)

### Session 4: Core HAL ✓

- [x] Implement `hal/esp32_gpio.c` — GpioInit, GpioSetInterrupt, GpioRemoveInterrupt, GpioWrite, GpioRead (using ESP-IDF gpio driver)
- [x] Implement `hal/esp32_spi.c` — SpiInit, SpiDeInit, SpiInOut (using ESP-IDF spi_master, SPI3_HOST, 10MHz, software CS)
- [x] Implement `hal/esp32_timer.c` — TimerInit, TimerStart, TimerStop, TimerSetValue, TimerGetCurrentTime, TimerGetElapsedTime (one esp_timer + sorted linked list, ESP_TIMER_TASK dispatch)
- [x] Implement `hal/esp32_delay.c` — DelayMs (vTaskDelay for >10ms, esp_rom_delay_us for short delays)
- [x] Compile clean — zero errors, zero warnings; firmware 1565 KB (no size regression)

### Session 5: Board and Radio HAL ✓

- [x] Implement `hal/esp32_board.c` — BoardInitPeriph, BoardInitMcu, BoardGetRandomSeed (esp_random), BoardGetUniqueId (WiFi MAC), BoardGetBatteryLevel
- [x] Implement `hal/sx1276_board.c` — SX1276IoInit, SX1276IoIrqInit, SX1276SetRfTxPower (PA_BOOST), SX1276Reset, SX1276GetPaSelect
- [x] Implement `hal/sx126x_board.c` — SX126xIoInit, SX126xIoIrqInit, SX126xReset, SX126xWaitOnBusy, SX126xWriteCommand/ReadCommand, SX126xWriteRegisters/ReadRegisters, SX126xWriteBuffer/ReadBuffer, SX126xSetRfTxPower, SX126xGetDeviceId, SX126xAntSwOn/Off; SX126xIoTcxoInit sets DIO3→1.8V TCXO; SX126xIoRfSwitchInit enables DIO2 RF switch
- [x] Compile clean — zero errors, zero warnings; firmware 1565 KB (no size regression)

### Session 6: Integration and Test ✓

- [x] Implement runtime radio selection: Radio_SX1276 and Radio_SX126x tables; radio_select.c owns writable Radio global; lorawan_radio_select(bool) copies the right table at init
- [x] Fix dual-radio symbol collision: hal/sx1276_radio_wrapper.c and hal/sx126x_radio_wrapper.c rename TxTimeoutTimer/RxTimeoutTimer/FskBandwidths/Radio via #define before #including originals — no modifications to loramac-node/ sources
- [x] Compile full HAL + both radio drivers — zero errors, zero warnings; firmware 1579 KB (both Radio tables now fully linked)
- [x] Test SPI: read register 0x42 = 0x12 confirmed on T-Beam SX1276 hardware
- [x] Test timer accuracy: 200ms timer fires within 500ms confirmed on hardware
- [x] Test on SX1262 board: BUSY pin reads, command interface, OTAA join, uplink, downlink — confirmed on T-Beam v1.1 SX1262/AXP192

## Phase 4 — Python Bindings (Sessions 7–10)

### Session 7: ABP Join + Send (first uplink!) ✓

- [x] Implement lorawan_obj_t struct in modlorawan.c
- [x] Implement LoRaWAN.__init__ (radio detection via reg 0x42, MAC init, FreeRTOS task)
- [x] Implement join_abp() — configure DevAddr, NwkSKey (all 3 LoRaWAN 1.1 slots), AppSKey
- [x] Implement send() — blocks up to 10 s waiting for McpsConfirm
- [x] Dedicated FreeRTOS task (lorawan_task, CPU1, priority 6) with cmd queue + event group
- [x] **CRITICAL**: EU868 RX2 set to 869.525 MHz / DR3 (SF9/BW125) for TTN — both MIB_RX2_CHANNEL and MIB_RX2_DEFAULT_CHANNEL
- [x] TCXO: handled at radio HAL level (SX1276: crystal 0x09 from CLAUDE.md; SX1262: DIO3→1.8V from Session 5)
- [x] Compile clean — zero errors, 1612 KB firmware; module constants EU868, CLASS_A/B/C exported
- [x] Fix three `portTICK_PERIOD_MS` integer-division bugs (2/10=0, 10000/10=1000 ticks): replaced with `pdMS_TO_TICKS()`
- [x] Guard `LoRaMacProcess()` with `s_mac_initialized` flag — must not call before `LoRaMacInitialization()`
- [x] Fix GIL deadlock: `mp_printf` from LoRaWAN task blocks on `MP_THREAD_GIL_ENTER()` — Python thread holds GIL while sleeping in `xEventGroupWaitBits`. Replaced with `esp_rom_printf` (ROM UART, no locking) throughout the task.
- [x] Fix task stack: ESP-IDF v5+ `xTaskCreatePinnedToCore` uses BYTES not words — was passing `8192/sizeof(StackType_t)=2048` bytes; now passes 8192 bytes directly. Silent overflow masked by busy-spin (canary never checked).
- [x] Fix busy-spin deadlock: `pdMS_TO_TICKS(10)` = 1 tick on 100 Hz — `pdMS_TO_TICKS(2)=0` caused non-blocking `ulTaskNotifyTake` which acquired `xKernelLock` at MHz rate, starving CPU0's `xQueueSend`.
- [x] Add `datarate` kwarg to `send()` — DR_0..DR_5 constants exported; default DR_0 (SF12) for maximum uplink range
- [x] Test: LoRaMacInitialization ✓, ABP join ✓, send() TX confirmed ✓ — packet transmitted on hardware
- [x] Test: ABP uplink received by TTN ✓ — TTN immediately scheduled a downlink (FCnt=0, UNCONFIRMED_DOWN)
- [x] Fix LoRaWAN 1.0.4 MIC: LoRaMAC-node defaults to 1.1.1 MIC (two-key); TTN ABP expects 1.0.x (single-key). Set MIB_ABP_LORAWAN_VERSION=1.0.4 in join_abp.

### Session 8: OTAA Join ✓

- [x] Implement join_otaa() — full join procedure with dev_eui, join_eui, app_key; retry loop via s_otaa_retry_needed flag; blocking wait with timeout parameter; raises OSError(ETIMEDOUT) on timeout
- [x] Implement joined() — returns bool (set by join_abp or mlme_confirm MLME_JOIN OK)
- [x] Implement stats() — returns dict with rssi, snr, tx_counter, rx_counter, tx_time_on_air
- [x] Handle join timeout (blocking with timeout parameter)
- [x] Add rx_counter to lorawan_obj_t; captured from McpsIndication.DownLinkCounter
- [x] Implement mlme_confirm: MLME_JOIN OK → joined=true + EVT_JOIN_DONE; FAIL → s_otaa_retry_needed for task-context retry
- [x] Compile clean — zero errors; firmware 1576 KB (no size regression)
- [x] Test: OTAA join ✓, send() uplink ✓, stats() correct (tx_counter=1, rssi=-109, snr=6)

### Session 9: Receive + Confirmed Messages ✓

- [x] Implement recv(timeout=10) — blocks on s_rx_queue; returns (data, port, rssi, snr) or None; timeout=0 polls without blocking
- [x] Confirmed uplink already wired (MCPS_CONFIRMED in CMD_TX); added last_tx_ack field to lorawan_obj_t, captured from McpsConfirm.AckReceived; reported in stats()
- [x] Implement on_rx(callback) — mcps_indication schedules lorawan_rx_trampoline via mp_sched_schedule; trampoline pops from s_rx_queue and calls callback(data, port, rssi, snr) in Python context
- [x] Implement on_tx_done(callback) — mcps_confirm schedules lorawan_tx_trampoline; arg is True (confirmed: ACK received; unconfirmed: frame sent) or False (failure)
- [x] Compile clean — zero errors, zero warnings; firmware 1614 KB
- [x] Test: receive scheduled downlink from TTN ✓ — recv(timeout=10) returned (b'\x02\x03\x04\x05', 1, -105, 8)
- [x] Test: confirmed uplink with ACK ✓ — stats()["last_tx_ack"] = True; on_tx_done callback fired with True
- [x] Test: on_rx callback ✓ — lambda fired with (b'\x10\x11\x12\x13\x14\x15', 1, -104, 8)
- [x] Test: on_tx_done callback ✓ — "tx_done: True" printed after unconfirmed send
- Note: dev_nonce_too_small on TTN without NVS persistence — fixed in Session 10 (auto-save on join + nvram_restore() at boot)

### Session 10: Persistence + ADR ✓

- [x] Implement nvram_save() — store LoRaMacNvmData_t blob to ESP32 NVS via MIB_NVM_CTXS
- [x] Implement nvram_restore() — restore from NVS, sync joined/DR/ADR/tx_power fields; raises OSError(ENOENT) if no saved session
- [x] Implement datarate(dr=None) — getter/setter for MIB_CHANNELS_DATARATE; ADR-adjusted DR updated in mcps_confirm
- [x] Implement adr(enabled=None) — getter/setter for MIB_ADR
- [x] Implement tx_power(power=None) — getter/setter for MIB_CHANNELS_TX_POWER (0=max, 7=min)
- [x] Compile clean — zero errors; firmware 1616 KB (2 KB growth for NVM + getter/setter code)
- [x] Test: reboot without re-join, frame counter continues — FCntUp=4 saved/restored, first uplink FCnt=5 accepted by TTN immediately
- [x] Test: ADR jumps DR_0→DR_5 on first downlink after lw.adr(True); lw.datarate() tracks correctly
- [x] Fix DevNonce auto-save: mlme_confirm(JOIN_OK) sets s_nvram_autosave_needed; lorawan_task saves NVM immediately after LoRaMacProcess() returns — DevNonce is never lost to a reset between join and the first Python nvram_save() call
- Note: nvram_save CRC fix required — LoRaMacHandleNvm runs lazily on next LoRaMacProcess tick; nvram_save recomputes all group CRCs explicitly before nvs_set_blob to avoid stale CRC causing RestoreNvmData to silently skip the Crypto group (FCntUp reset to 0)
- Note: nvram_restore() must be called at every boot before join() — the MAC initialises DevNonce to 0 by default; without restore, each reboot repeats already-seen nonces and TTN rejects with "devnonce is too small"

## Phase 5 — Advanced Features (Sessions 11–15)

### Session 11: Core extras

- [x] Full runtime pin configuration from Python (for non-T-Beam boards)
      - New kwargs: `radio="sx1276"|"sx1262"`, `spi_id`, `mosi`, `miso`, `sclk`, `cs`, `reset`, `irq`, `busy`
      - `g_lorawan_pins` struct in `hal/pin_config.h/.c` — defaults are T-Beam values
      - All board IoInit functions use runtime struct instead of `lorawan_config.h` macros
      - `irq` sets both `dio0` (SX1276) and `dio1_1262` (SX1262) — only the right one is consumed
      - `radio` kwarg now accepts strings "sx1276"/"sx1262" (True/False kept for backwards compat)
      - QSTR promotion: clean build required after adding `busy` kwarg (stale frozen_content.c)
      - Compile clean — 1617 KB (1 KB growth for pin_config.c + new kwargs)
- [x] LoRaWAN version selection (`lorawan_version=lorawan.V1_0_4|lorawan.V1_1`)
      - `V1_0_4`, `V1_1` module constants exported
      - ABP: `MIB_ABP_LORAWAN_VERSION` set from instance version (1.0.4 single-key MIC; 1.1 two-key)
      - OTAA: `join_otaa(..., nwk_key=...)` optional kwarg; omit for 1.0.x (NwkKey=AppKey internally)
- [x] Configurable RX2 window (`rx2_dr`, `rx2_freq` kwargs on `__init__`)
      - Default: 869.525 MHz / DR_3 (TTN EU868). Pass `rx2_dr=lorawan.DR_0` for standard LoRaWAN.
      - Values applied in CMD_INIT from `lorawan_obj_t` instead of hardcoded constants
- [x] TX power in dBm EIRP (`tx_power=14` kwarg on `__init__`; getter/setter also in dBm)
      - `tx_power=None` → region max (16 dBm EU868). `tx_power=14` → 14 dBm (index 1).
      - `dbm_to_tx_power()` and `tx_power_to_dbm()` helpers: EU868 step = 2 dBm, range 2–16 dBm
      - Setter rounds down (never exceeds requested EIRP); getter converts index→dBm
- [x] Hardware TX power override (beyond regulatory limits, user responsibility)
      - `lw.tx_power(20)` or `tx_power=20` on init: `g_tx_power_hw_override` in pin_config.c/.h
      - `SX126xSetRfTxPower` and `SX1276SetRfTxPower` both check override before using MAC value
      - Hardware caps enforced by radio drivers: SX1276 ≤ 20 dBm, SX1262 ≤ 22 dBm
      - Override and ADR mutually exclusive: setting override auto-disables ADR; enabling ADR clears override
      - Warning logged when override exceeds region regulatory limit
      - Version bumped to 0.6.0; compile clean — 1618 KB (+1 KB)
- [x] End-to-end SX1262 test: join, uplink, downlink, confirmed, persistence — fixed sleep/wakeup bug (SX126xWakeup must update operating_mode to STDBY_RC; recursive SPI mutex; ensure_awake() in all SPI functions)
- [x] Class C support — `request_class(CLASS_A|CLASS_C)`, `device_class()`, `on_class_change(cb)`
      - Class A <-> C via `MIB_DEVICE_CLASS` (instantaneous, synchronous). Dispatched through new `CMD_SET_CLASS` in the LoRaWAN task so the MAC is touched only from task context. No MLME confirm fires for this transition — task schedules `on_class_change(new_class)` via `mp_sched_schedule` immediately after the MIB set succeeds.
      - `EVT_CLASS_OK` / `EVT_CLASS_ERROR` event bits mirror the NVRAM pattern for blocking `request_class()`. `class_callback` stored on `lorawan_obj_t`, init'd to `mp_const_none`.
      - `CLASS_B` raises `NotImplementedError` (needs beacon acquisition, Session 13).
      - Persistence: `MacGroup2.DeviceClass` is part of the NVM blob; `nvram_restore()` now reads `MIB_DEVICE_CLASS` back and syncs `self->device_class`. `LoRaMacInitialization()` does not force Class A — the restored class is honoured.
      - Version 0.6.0 → 0.7.0; compile clean, zero warnings; firmware 1619 KB.
      - **RxC window fix**: on first test, class C switched OK but `recv(timeout=30)` returned None despite TTN queueing a confirmed downlink. Root cause: LoRaMAC-node v4.7.0 has a **separate** `RxCChannel` (used by the Class C continuous listen) alongside `Rx2Channel`. `MIB_RX2_CHANNEL` only writes `Rx2Channel`. In `SwitchClass(CLASS_C)`, `OpenContinuousRxCWindow()` recomputes the RX config from `RxCChannel.Datarate` — which defaults to `PHY_DEF_RX2_DR` = DR_0 (SF12) in EU868 — so the radio ends up listening at SF12 while TTN transmits on DR_3 (SF9). Fix: `CMD_INIT` now also writes `MIB_RXC_CHANNEL` and `MIB_RXC_DEFAULT_CHANNEL` with the same (freq, DR) as RX2.
      - Test: configure device as Class C on TTN, call `request_class(CLASS_C)`, schedule a downlink from TTN console and verify `recv()` gets it without a preceding uplink. Also test class persistence across reboot.
- [x] Configurable antenna gain (`antenna_gain` kwarg on `__init__`, default 0.0)
      - EU868 region default is 2.15 dBi (`EU868_DEFAULT_ANTENNA_GAIN` in `RegionEU868.h`), which makes the MAC under-drive the radio by 2.15 dB vs. the requested EIRP. With default 0.0 the radio emits the full EIRP (`tx_power()` index 0 → 16 dBm actually transmitted).
      - `antenna_gain=<float>` kwarg on `__init__`; stored in `lorawan_obj_t.antenna_gain`. In `CMD_INIT` (after `LoRaMacStart`) both `MIB_ANTENNA_GAIN` and `MIB_DEFAULT_ANTENNA_GAIN` are set, so any `ResetMacParameters` keeps our value.
      - Getter/setter `lw.antenna_gain()` / `lw.antenna_gain(2.15)`: setter dispatches through `CMD_SET_PARAMS` type 3 (extended the union with a `float` field) and updates both MIB slots. `nvram_restore()` syncs the Python cache from `MIB_ANTENNA_GAIN`.
      - Version bumped 0.7.0 → 0.8.0; compile clean, firmware 1582 KB (no regression).
- [x] EU433 region support (enable REGION_EU433 in config)
      - `REGION_EU433` compile def added to `micropython.cmake`; `RegionEU433.c` added to source list
      - `lorawan.EU433` module constant already exported (pre-existing)
      - RX2 default simplified: `__init__` no longer forces any RX2 override. Without `rx2_dr`/`rx2_freq` kwargs, the MAC uses the region's `PHY_DEF_RX2` (standard LoRaWAN DR_0 / region default freq). Each kwarg is independent; TTN EU868 users typically just pass `rx2_dr=DR_3` because 869.525 MHz is already the EU868 region default
      - `CMD_INIT` GETs the current `MIB_RX2_CHANNEL` (populated by `LoRaMacStart` from region defaults), overrides whichever field the user set, and writes it back. Sentinels: `rx2_freq=0`, `rx2_dr=0xFF` mean "not overridden"
      - Known limitation: `tx_power_to_dbm`/`dbm_to_tx_power` helpers assume the EU868 EIRP table (max 16 dBm, step -2 dB). EU433 `DEFAULT_MAX_EIRP` is 12.15 dBm (see `RegionEU433.h:115`), so `lw.tx_power()` returns/accepts EU868 dBm values on EU433 hardware. Acceptable for now — region-accurate dBm↔index mapping is a future refactor
      - Compile clean — firmware 1584 KB (+2 KB for RegionEU433.c)
      - Hardware test: pending — requires EU433 radio and gateway

### Session 12: Advanced MAC commands + time sync

- [x] Implement `request_device_time()` — MLME_DEVICE_TIME, sends DeviceTimeReq
      - New `CMD_REQUEST_DEVICE_TIME` dispatched through the LoRaWAN task; the
        MLME request only queues the DeviceTimeReq as a piggy-back MAC command,
        so the caller must follow with `send()` to carry it over the air.
      - `mlme_confirm()` gains a `MLME_DEVICE_TIME` case: on status OK it reads
        `SysTimeGet()` (MAC has already applied `SysTimeSet` during RX), converts
        to GPS epoch seconds (Unix - `UNIX_GPS_EPOCH_OFFSET`) and stores it on
        `lorawan_obj_t.network_time_gps`, flagging `time_synced=true` for the
        upcoming `network_time()` / `on_time_sync()` APIs.
      - Version bumped 0.8.0 → 0.9.0; compile clean, firmware 1624 KB.
- [x] Implement `network_time()` — returns GPS epoch seconds from last DeviceTimeAns
      - Reads the `network_time_gps` snapshot captured in `mlme_confirm(MLME_DEVICE_TIME)`.
        Returns `None` when `time_synced` is still false (no answer received yet).
- [x] Implement `on_time_sync(callback)` — notification when time is received
      - `time_sync_callback` added to `lorawan_obj_t`; registered via `on_time_sync(cb)`.
      - `mlme_confirm(MLME_DEVICE_TIME, OK)` schedules `lorawan_time_sync_trampoline`
        via `mp_sched_schedule`. The trampoline reads `network_time_gps` from the
        object and allocates the Python int in VM context — the GPS epoch in 2026
        (~1.4 B) exceeds the 31-bit `MP_OBJ_NEW_SMALL_INT` range, so passing it
        through the scheduler arg would overflow.
      - Version bumped 0.9.0 → 0.9.1; compile clean, firmware 1624 KB (unchanged).
- [x] Implement `link_check()` — MLME_LINK_CHECK, returns margin + gateway count
      - New `CMD_REQUEST_LINK_CHECK` dispatched through the LoRaWAN task; the
        MLME request only queues the LinkCheckReq as a piggy-back MAC command,
        so the caller must follow with `send()` to carry it over the air.
      - `mlme_confirm()` gains a `MLME_LINK_CHECK` case: on status OK it caches
        `DemodMargin` and `NbGateways` on `lorawan_obj_t`, flagging
        `link_check_received=true` after the first successful answer.
      - `link_check()` queues the MLME and returns the cached answer as
        `{"margin": N, "gw_count": N}` (or `None` if no LinkCheckAns yet).
        Typical pattern: `lw.link_check(); lw.send(b"p"); result = lw.link_check()`.
      - Compile clean, firmware 1624 KB (unchanged).
- [x] Implement `rejoin(type=0|1|2)` — LoRaWAN 1.1 rejoin without full join
      - New `CMD_REQUEST_REJOIN` dispatched through the LoRaWAN task; maps
        type 0/1/2 to `MLME_REJOIN_0/1/2` and calls `LoRaMacMlmeRequest`.
      - Unlike `link_check()` / `request_device_time()` (piggy-back), a
        rejoin is itself an uplink (`SendReJoinReq` → `ScheduleTx`), so no
        follow-up `send()` is needed.
      - `mlme_confirm` logs `MLME_REJOIN_*` outcomes; the optional
        Join-Accept (type 1) is processed internally by the MAC and
        re-derives session keys.
      - Python API: `lw.rejoin()` (defaults to type 0), `lw.rejoin(1)`,
        `lw.rejoin(2)`. Raises `RuntimeError` if not joined, `ValueError`
        if type is outside 0–2. Meaningful only for LoRaWAN 1.1.
      - Version bumped 0.9.1 → 0.9.2; compile clean, firmware 1624 KB.
- [x] Register Clock Sync package (LmhpClockSync, port 202)
      - Added `bindings/lmhandler_shim.c/.h` — minimal LmHandler adapter that
        provides the three functions LmhpClockSync.c calls: `LmHandlerIsBusy()`
        (queries `LoRaMacQueryTxPossible`), `LmHandlerSend()` (wraps
        `LoRaMacMcpsRequest` using the current `MIB_CHANNELS_DATARATE`), and
        `LmHandlerPackageRegister()` (stores the package and wires
        `OnDeviceTimeRequest` → `MLME_DEVICE_TIME` + `OnSysTimeUpdate` → our
        `lorawan_on_sys_time_update` hook). The full `LmHandler.c` is NOT
        linked: it carries its own `LoRaMacInitialization` path and would
        collide with our existing primitives/task design.
      - `LmhpClockSync.c` compiled in from the vendored loramac-node tree; new
        include paths `.../LmHandler` and `.../LmHandler/packages` added to
        `micropython.cmake`.
      - `mcps_confirm` / `mcps_indication` in modlorawan.c now call
        `lorawan_packages_on_mcps_confirm/indication()` first so registered
        packages see MCPS events before the standard user-port filter.
- [x] Implement `clock_sync_enable()`, `clock_sync_request()`, `synced_time()`
      - `clock_sync_enable()`: dispatched via `CMD_CLOCK_SYNC_ENABLE` so the
        package registration runs on the LoRaWAN task (single-thread MAC
        ownership). Sets `lorawan_obj_t.clock_sync_enabled` on success.
      - `clock_sync_request()`: dispatched via `CMD_CLOCK_SYNC_REQUEST`,
        wraps `LmhpClockSyncAppTimeReq()`. Unlike `request_device_time()`
        (which only piggy-backs), this already emits an uplink on port 202 —
        no follow-up `send()` needed. Raises `RuntimeError` if not joined or
        if the package has not been enabled yet.
      - `synced_time()`: returns live `SysTimeGet().Seconds - UNIX_GPS_EPOCH_OFFSET`
        (GPS epoch) when time has been synced via either the AppTimeAns or
        the MAC-level DeviceTimeAns path. Returns `None` before the first
        sync. Complements `network_time()` which returns the snapshot taken
        at the moment of last sync.
      - Shim's `OnSysTimeUpdate` hook updates `self.time_synced` and
        `self.network_time_gps` and schedules the existing
        `lorawan_time_sync_trampoline`, so `on_time_sync(cb)` fires for both
        the DeviceTimeReq and the AppTimeReq paths with identical semantics.
      - Version bumped 0.9.2 → 0.9.3; compile clean, firmware 1626 KB
        (+2 KB for the shim + LmhpClockSync.c).
- [ ] Test: DeviceTimeReq on TTN, verify epoch matches real time
- [ ] Test: LinkCheckReq, verify gateway count and margin
- [ ] Test: Clock sync time correction

### Session 13: Class B (beacons + ping slots)

Prerequisites: DeviceTimeReq must work (Session 12), timer HAL accuracy verified (Session 6).

- [x] Enable `LORAMAC_CLASSB_ENABLED` in `micropython.cmake` so `LoRaMacClassB.c` compiles in; otherwise every Class B entry point in `LoRaMac.c` returns `LORAMAC_STATUS_SERVICE_UNKNOWN` silently.
- [x] Implement HAL `BoardGetTemperatureLevel()` → returns fixed 25.0 °C. Wired via `s_mac_callbacks.GetTemperatureLevel`. Used by `LoRaMacClassB.c: ApplyTemperatureDrift` for crystal-drift compensation on beacon timing; the Nucleo reference ports use the same fixed estimate when a sensor is absent.
- [x] Implement `request_class(CLASS_B)` — async, multi-step. `CMD_SET_CLASS` gates on `time_synced` + current class == CLASS_A, then fires `MLME_BEACON_ACQUISITION` and returns immediately (EVT_CLASS_OK). The follow-up is driven by `s_classb_*` flags flipped in MLME callback context and drained from the task loop (same pattern as the OTAA retry):
    - `MLME_BEACON_ACQUISITION` confirm OK → `s_classb_need_ping_slot_req` → task issues `MLME_PING_SLOT_INFO` + empty-port-0 MCPS_UNCONFIRMED uplink (piggy-back flush, mirrors `LmHandlerPingSlotReq`).
    - `MLME_PING_SLOT_INFO` confirm OK → `s_classb_need_switch_class` → task sets `MIB_DEVICE_CLASS = CLASS_B` (now accepted because `BeaconMode==1 && PingSlotCtx.Assigned==1`) and schedules `on_class_change(CLASS_B)`.
    - `MLME_BEACON_ACQUISITION` FAIL → `s_classb_need_device_time_req` → task re-issues `MLME_DEVICE_TIME` + `MLME_BEACON_ACQUISITION` to re-sync the search window.
- [x] Implement `ping_slot_periodicity(N)` — `N=0..7`, period `2^N` s. Getter/setter; value stored on the object and applied on the next `request_class(CLASS_B)` (changing it while already in Class B requires a renegotiation).
- [x] Implement `on_beacon(callback)` — `callback(state, info)` where state ∈ {`BEACON_ACQUISITION_OK`, `BEACON_ACQUISITION_FAIL`, `BEACON_LOCKED`, `BEACON_NOT_FOUND`, `BEACON_LOST`} and info is either the beacon dict (when a beacon has been received) or `None`. Fires on every `MLME_BEACON` / `MLME_BEACON_LOST` indication and on `MLME_BEACON_ACQUISITION` confirm outcomes.
- [x] Implement `beacon_state()` — returns `{state, time (GPS epoch s), freq (Hz), datarate, rssi, snr, gw_info_desc, gw_info (6-byte bytes)}` or `None` if no beacon received yet. Snapshot captured in `mlme_indication(MLME_BEACON, BEACON_LOCKED)`; `BEACON_LOST` clears `beacon_info_valid`.
- [x] `device_class()` already existed (Session 11) — no change needed.
- [x] Handle `MLME_BEACON_LOST` — sets `MIB_DEVICE_CLASS = CLASS_A`, clears all `s_classb_*` flags, clears `beacon_info_valid`, schedules `on_beacon(LOST)` + `on_class_change(CLASS_A)`. This is the real "2h without beacon" revert path; `MLME_REVERT_JOIN` (LoRaWAN 1.1 rekey revert, unrelated to beacons) is also handled — the session is invalidated (`joined=false`) so the user must rejoin.
- [x] New module constants exported: `BEACON_ACQUISITION_OK`, `BEACON_ACQUISITION_FAIL`, `BEACON_LOCKED`, `BEACON_NOT_FOUND`, `BEACON_LOST`.
- [x] Version bumped 0.9.3 → 0.10.0; compile clean, zero warnings; firmware 1632 KB (+8 KB for `LoRaMacClassB.c`).
- [ ] Test: DeviceTimeReq → request Class B → beacon locked → receive on ping slot (TTN gateway needs Class B beacon support — many community gateways don't).
- [ ] Test: beacon loss → reacquisition → automatic revert after timeout

### Session 14: Multicast (local + remote, Class B + C)

- [x] Implement `multicast_add(group, addr, mc_nwk_s_key, mc_app_s_key, f_count_min=0, f_count_max=0xFFFFFFFF)` — local setup, up to 4 groups
      - Dispatched via `CMD_MC_ADD` so `LoRaMacMcChannelSetup()` runs from task context with `IsRemotelySetup=false`. The MAC copies the session keys into the soft-SE immediately, so our stack-local key buffers don't need to outlive the call.
      - Mirror fields (addr, fcnt range, is_remote=false) are kept on `lorawan_obj_t.mc_groups[0..3]` for `multicast_list()`; the MAC owns the authoritative copy in `Nvm.MacGroup2.MulticastChannelList` but exposes no getter.
- [x] Implement `multicast_rx_params(group, device_class, frequency, datarate, periodicity=0)` — Class B (with periodicity) or Class C
      - Dispatched via `CMD_MC_RX_PARAMS`; builds `McRxParams_t` with the Class B/C union selected by `device_class`. Python raises `ValueError` if the class is anything other than `CLASS_B` / `CLASS_C` (multicast does not apply to Class A).
- [x] Implement `multicast_remove(group)` — delete group
      - `CMD_MC_REMOVE` → `LoRaMacMcChannelDelete()` and clears the local mirror.
- [x] Implement `multicast_list()` — returns active groups with metadata
      - Returns a list of dicts `{group, addr, is_remote, f_count_min, f_count_max, device_class?, frequency?, datarate?, periodicity?}`. RX-param keys only appear after `multicast_rx_params()` has been called for the group.
- [x] Ensure `on_rx(callback)` receives multicast downlinks with group info in metadata
      - `lorawan_rx_pkt_t` gained a `multicast` flag fed from `McpsIndication.Multicast`. The Python tuple is now `(data, port, rssi, snr, multicast)` — 5-tuple is a breaking change from the 4-tuple in Sessions 9–13 but the multicast flag has to live somewhere and the dict option adds per-packet allocation overhead. `recv()` still returns the 4-tuple (keeping backwards compatibility for the polling path); only `on_rx` callbacks see the new shape.
- [x] Register Remote Multicast Setup package (LmhpRemoteMcastSetup) for server-driven configuration
      - Extended `lmhandler_shim.c`: new `LmHandlerPackageRegister(PACKAGE_ID_REMOTE_MCAST_SETUP)` branch plus a shim implementation of `LmHandlerRequestClass()` (direct `MIB_DEVICE_CLASS` set, safe to call from package `Process()` which runs in LoRaWAN task context).
      - Added `lorawan_packages_process()` — called from the task main loop each iteration so Remote Mcast Setup can run its session-start / session-stop state transitions.
- [x] Implement `remote_multicast_enable()` — server can then send McGroupSetupReq
      - `CMD_REMOTE_MCAST_ENABLE` registers the package from task context; Python returns the resulting `remote_mcast_enabled` bool.
      - `McGroupSetupReq`, `McGroupDeleteReq`, `McGroupClassCSessionReq` and `McGroupClassBSessionReq` are handled entirely by `LmhpRemoteMcastSetup.c` — no extra modlorawan.c glue needed. After session start the device switches to the negotiated class automatically via the shim's `LmHandlerRequestClass`.
- [x] Register Fragmentation package (LmhpFragmentation) with progress/done callbacks
      - Shim owns the FragDecoder memory-backed read/write callbacks (`FRAG_DECODER_FILE_HANDLING_NEW_API == 1`): a flat RAM buffer that `frag_decoder_write()` populates and `frag_decoder_read()` serves.
      - `LmhpFragmentationParams_t` is populated with pointers to C-side `OnProgress` / `OnDone` adapters that forward to `lorawan_on_fragmentation_progress/done` in modlorawan.c. Those adapters snapshot arguments onto the active `lorawan_obj_t` and `mp_sched_schedule` the Python trampoline (allocation of the Python tuple happens in VM context).
- [x] Implement `fragmentation_enable(buffer_size, on_progress, on_done)` — for FUOTA
      - `CMD_FRAGMENTATION_ENABLE` mallocs the requested buffer (raw C heap, outside MicroPython's GC since the MAC writes to it from the LoRaWAN task) and calls `lorawan_fragmentation_register()`. Rejects a second call while already enabled (second-call would leak the previous buffer).
      - `fragmentation_data()` returns the reassembled buffer as `bytes` after `on_done` has fired; caller slices to the reported size.
- [x] Version bumped 0.10.0 → 0.11.0; compile clean, zero warnings; firmware 1643 KB (+11 KB for LmhpRemoteMcastSetup.c + LmhpFragmentation.c + FragDecoder.c + Session 14 Python glue).
- [x] Test: local multicast Class C — device receives on multicast address
- [ ] Test: local multicast Class B — device receives on multicast ping slot
- [ ] Test: remote multicast setup via LmhpRemoteMcastSetup (if server supports it)
- [ ] Test: fragmentation session end-to-end (server pushes fragments, `on_done` fires with reassembled payload)

### Session 15: Documentation and examples ✓

- [x] API reference documentation — `README.md` rewritten with full Phase 5 coverage: Class A/B/C, time sync (DeviceTimeReq + Clock Sync package), LinkCheckReq, ReJoin, multicast (local + remote), fragmentation/FUOTA, beacon state, ping-slot periodicity. Fixed two pre-existing errors from Phase 4 docs: `joined()` is a method not an attribute, and the `on_rx` callback now receives a 5-tuple `(data, port, rssi, snr, multicast)` (multicast flag added in Session 14).
- [x] Hardware setup guide — new section covering LoRa antenna (damage risk without), GPS antenna, power, and TTN registration walkthrough (frequency plan "Europe 863–870 MHz (SF9 for RX2)" maps to the default `rx2_dr=DR_3`).
- [x] Example scripts: `lorawan-module/examples/basic_otaa.py`, `basic_abp.py`, `sensor_node.py` — all gated by `tbeam.detect()`, print diagnostic info (RSSI/SNR/counters), handle OSError on join timeout and RuntimeError on send failure. `sensor_node.py` uses `machine.deepsleep()` with pre-sleep `nvram_save()` and reads AXP192 battery voltage inline when available.
- [x] Example scripts: `time_sync.py`, `class_b_beacon.py`, `multicast_receiver.py` — all demonstrate the relevant callback (`on_time_sync`, `on_beacon`, `on_rx`-with-multicast-flag). `class_b_beacon.py` walks the full flow: join → DeviceTimeReq → set ping-slot periodicity → request CLASS_B → wait for `BEACON_LOCKED` state. `multicast_receiver.py` uses local group provisioning (keys in-script) and Class C continuous listen.
- [x] `lorawan-module/examples/README.md` index with a pointer to each script and notes on LNS prerequisites (Class B beacon support, multicast API availability).

### Session 16: API polish (deinit, soft-reset safety, missing getters)

Gaps found while reviewing the Python surface against the MAC capabilities. The `deinit` path is the reported pain point: after `Ctrl-D` in the REPL, `s_task_handle` stays alive (the FreeRTOS task is outside the MicroPython heap) while `s_lora_obj` becomes a dangling pointer to freed memory — the next RX/timer callback dereferences garbage and may crash. The rest are small but load-bearing: the MAC already knows the answers, we just don't expose them.

#### Lifecycle

- [x] `lw.deinit()` — ordered teardown dispatched through a new `CMD_DEINIT`. The LoRaWAN task calls `LoRaMacDeInitialization()` (falling back to `lorawan_radio_sleep()` if the MAC is mid-TX/RX), drops LmHandler package registrations and the FUOTA buffer (`lorawan_packages_deinit()`), runs `SX1276IoDeInit()` / `SX126xIoDeInit()` to deregister DIO ISRs + free the SPI bus, calls new `lorawan_timer_deinit()` (stops+deletes the shared `esp_timer` backing the LoRaMAC timer list) and `sx126x_spi_mutex_deinit()`, then `vTaskDelete(NULL)`. Python side then deletes `s_cmd_queue` / `s_rx_queue` / `s_events` and clears all statics. Idempotent. After `deinit()` a fresh `lorawan.LoRaWAN(...)` works in the same REPL session.
- [x] Soft-reset hook — implemented via finaliser-backed allocation (`mp_obj_malloc_with_finaliser`) + `{ __del__ → deinit }` in locals_dict. `gc_sweep_all()` (ports/esp32/main.c:212) runs during soft-reset and fires the finaliser automatically — matches the pattern already used by `machine.I2S`, `machine.I2CTarget`, `modtls` etc. Avoids having to touch `ports/esp32/main.c` with a lorawan-specific deinit call (ESP32 port has no `MICROPY_BOARD_START_SOFT_RESET` hook).
- [x] `__enter__` / `__exit__` — `__enter__` returns self; `__exit__` delegates to `deinit()` regardless of exception state. `with lorawan.LoRaWAN(...) as lw:` now guarantees cleanup on scope exit.

#### Missing getters (MIB already has the answer)

- [x] `lw.dev_addr()` — returns the current 32-bit DevAddr from `MIB_DEV_ADDR`. Dispatched via new `CMD_QUERY` (sub-type `LW_QUERY_DEV_ADDR`) so the MIB read happens on the LoRaWAN task, keeping single-threaded MAC ownership. Result staged in `s_query_dev_addr` while the Python caller is blocked on `EVT_COMPLETED`. Returns 0 before any activation.
- [x] `lw.region()` — returns `self->region` directly (`LORAMAC_REGION_*` value cached at `__init__`). No CMD round-trip.
- [x] `lw.max_payload_len()` — wraps `LoRaMacQueryTxPossible(0, &tx_info)` via `CMD_QUERY` (sub-type `LW_QUERY_MAX_PAYLOAD_LEN`); returns `tx_info.MaxPossibleApplicationDataSize`. Treats both `LORAMAC_STATUS_OK` and `LORAMAC_STATUS_LENGTH_ERROR` as success since both populate the field. Lets callers slice payloads before `send()` instead of catching a late `LENGTH_ERROR`.
- [x] Version bumped 0.13.0 → 0.14.0; compile clean, zero warnings; firmware 1645504 bytes (+288 B).

#### Duty cycle

Confirmed: duty cycle IS enforced. `EU868_DUTY_CYCLE_ENABLED = 1` in `RegionEU868.h:125` is applied at init via `MacGroup2.DutyCycleOn`. Every uplink past the first is gated by the regional band timers.

The reason back-to-back `send()` calls *appear* to ignore the duty cycle is a hard-coded 10 s wait in `send()`: `xEventGroupWaitBits(..., pdMS_TO_TICKS(10000))` at `modlorawan.c:2171`. In LoRaMAC-node v4.7.0, when the MAC is restricted it returns `LORAMAC_STATUS_OK` (not an error) and schedules an internal `TxDelayedTimer` to fire when the band clears — typically tens of seconds on EU868 at low DR. Our `send()` gives up at 10 s with `RuntimeError("send timeout")` while the frame is still queued inside the MAC, and it transmits later *on its own* (user sees `tx_counter` advance with no Python call in between).

- [x] `send(..., timeout=...)` — `timeout` kwarg added. Default 120 s (covers a duty-cycled DR_0 uplink in one call); `timeout=None` blocks until TX completes via `portMAX_DELAY`; non-positive ints also map to "wait forever" so user code that does `timeout=0` does not silently get the old 10 s behaviour. Same `RuntimeError("send timeout")` on cap; the MAC may still have the frame queued internally and emit it later (`tx_counter` advances).
- [x] `lw.duty_cycle([enabled])` — getter returns the cached `Nvm.MacGroup2.DutyCycleOn` (read at init via `MIB_NVM_CTXS` and after `nvram_restore()`). Setter dispatches `CMD_SET_PARAMS` type 4 → `LoRaMacTestSetDutyCycleOn(bool)` (no `MIB_CHANNELS_DUTY_CYCLE` exists in v4.7.0; the test API is the only public flip). On by default for EU868/EU433 region defaults; disabling logs a non-conformance warning.
- [x] `lw.time_until_tx()` — returns ms until the next TX is allowed. Reads `last_dc_wait_ms` captured from `mcps.ReqReturn.DutyCycleWaitTime` after every `LoRaMacMcpsRequest` and subtracts `esp_timer_get_time()` elapsed since. Returns 0 before the first send, when duty cycle is off, or once the wait window has elapsed.
- [x] README — document the current duty-cycle behaviour (enforced, async via `TxDelayedTimer`, why pre-Session-16 `send()` 10 s timeouts appeared suspicious) alongside the new `timeout` kwarg, `duty_cycle()` and `time_until_tx()`. New "Duty cycle" section + `send()` signature updated with `timeout=120` default; `timeout=None` documented as "block forever".

#### Channel management (optional — needed if anyone deploys outside TTN defaults)

- [ ] `lw.add_channel(index, freq, dr_min, dr_max)` — wraps `MIB_CHANNELS` + `LoRaMacChannelAdd`. Only meaningful on regions with dynamic channels (EU868, EU433).
- [ ] `lw.remove_channel(index)` — `LoRaMacChannelRemove`.
- [ ] `lw.channels()` — list of active channels with their params (from `MIB_CHANNELS`).

#### Stats expansion

- [x] Extend `stats()` dict with `last_tx_dr`, `last_tx_freq`, `last_tx_power` (snapshot captured in `mcps_confirm` from `McpsConfirm.Datarate / TxPower / Channel`). The `Channel` field is an index into the region channel list — resolved to Hz via `MIB_CHANNELS` inside `mcps_confirm` (task context, safe). `last_tx_power` is converted from MAC index to dBm via `tx_power_to_dbm()` for consistency with the live `tx_power()` getter. Distinct from the live `datarate()` / `tx_power()` getters: those report the *next* TX values after any ADR adjustment in the same confirm, which can mask what was just sent. All three default to 0 before the first TX (mirrors the existing `tx_counter == 0` convention).
- [x] Version bumped 0.14.0 → 0.15.0; compile clean, zero warnings; firmware 1645696 bytes (+192 B).

#### Documentation

- [x] README — new "Lifecycle" section covering `deinit()`, the soft-reset caveat (GC sweep + finaliser pattern so `Ctrl-D` is safe) and the `with lorawan.LoRaWAN(...) as lw:` context manager.
- [x] README — note the duty-cycle default and how to disable for bench testing. Covered in the new "Duty cycle" section: EU868/EU433 enforced by default, `TxDelayedTimer` explanation, `duty_cycle(False)` disable path with non-conformance warning for private-gateway use.
- [x] Version bump 0.11.0 → 0.12.0 on lifecycle completion (lorawan.version() returns '0.12.0'). Compile clean, zero warnings; firmware 1644624 bytes.
- [x] Version bump 0.12.0 → 0.13.0 on duty-cycle additions (`send(timeout=...)`, `duty_cycle()`, `time_until_tx()`). Compile clean, zero warnings; firmware 1645216 bytes (+592 B).

### Session 18: DR control on MAC-command uplinks + explicit link_check send mode

Two UX gaps surfaced from field testing on TTN at the EU868 cell edge:

- [x] `clock_sync_request(*, datarate=None)` — optional DR override for the single
      AppTimeReq uplink (port 202). When ADR ratchets the DR up to SF7, range-limited
      AppTimeReq frames silently fail to reach the gateway (`MLME_DEVICE_TIME
      status=4`). `LmhpClockSync` already snapshots and restores `MIB_CHANNELS_DATARATE`
      around the MCPS confirm (`LmhpClockSync.c:339-341 / 205-208`), so the override
      only affects that one AppTimeReq and does not leak into subsequent user uplinks.
      New `dr_override` field on the cmd union carries the DR into the task loop;
      the CMD_CLOCK_SYNC_REQUEST handler calls `LoRaMacMibSetRequestConfirm` before
      `lorawan_clock_sync_app_time_req()`. `None` = keep current MIB (previous behaviour).
- [x] `link_check(*, send_now=False, datarate=None)` — opt-in explicit uplink. The
      previous `link_check()` only queued `MLME_LINK_CHECK` as a piggy-back, so calling
      it repeatedly without a subsequent `send()` returned `None` forever — confusing
      UX (the function is named like a synchronous probe). The piggy-back-only default
      is preserved for callers that want to cheaply attach the MAC command to the next
      telemetry uplink; `send_now=True` now also emits an empty unconfirmed port-1
      frame, blocks on `EVT_TX_DONE`, and returns the fresh `{margin, gw_count}` in a
      single call. `datarate=` is only meaningful with `send_now=True`; defaults to
      `self->channels_datarate` (current MIB) when omitted. Raises `RuntimeError` on
      `EVT_TX_ERROR` or 120 s timeout.
- [x] README — both signatures updated with the new kwargs, including a worked
      example of `link_check(send_now=True, datarate=DR_0)` and a note on the
      behavioural change (default is still piggy-back-only; `send_now` is opt-in).
- [x] `examples/time_sync.py` — new section demonstrating `clock_sync_request(datarate=DR_0)`
      for robust cell-edge sync + an inline `link_check(send_now=True, datarate=DR_0)`
      showing both the fresh answer and the airtime/FCntUp cost.
- [x] Version bumped 0.16.0 → 0.17.0; compile clean, firmware 1646240 bytes.

### Session 17: Teardown ordering hardening (deinit safety)

Follow-up to Session 16's hazard note ("An IRQ ISR firing mid-teardown will corrupt state"). Review of the `CMD_DEINIT` path found three subtle issues worth tightening before shipping the lifecycle story.

- [x] DIO ISRs now deregistered **before** `LoRaMacDeInitialization()`. New `SX1276IoIrqDeInit()` / `SX126xIoIrqDeInit()` in the HAL board files call `GpioRemoveInterrupt` on DIO0/DIO1 (SX1276) or DIO1 (SX1262) only — SPI stays up so the MAC and the `lorawan_radio_sleep()` fallback can still command the radio. Called from the `CMD_DEINIT` handler as step 1. The `GpioRemoveInterrupt` inside `SX*IoDeInit` stays as an idempotent defensive no-op (see `esp32_gpio.c:84` — calling on an already-disabled pin is a no-op).
- [x] Bounded-wait blindness: `lorawan_deinit` (Python side) now captures the return of `xEventGroupWaitBits(EVT_COMPLETED, ..., 2000 ms)`. On timeout it logs via `mp_printf` and returns **without** deleting `s_cmd_queue` / `s_rx_queue` / `s_events`. A small leak is strictly better than deleting primitives while the LoRaWAN task may still be dereferencing them.
- [x] Rejected `eTaskGetState` polling as a "safer" alternative to the `vTaskDelay(10 ms)` that lets the idle task reclaim the task's TCB — the FreeRTOS implementation of `eTaskGetState` dereferences the handle directly and would UAF as soon as the idle task on CPU1 frees the TCB between polling iterations. The 10 ms delay stays, with a comment explaining why.
- [x] Version bumped 0.15.0 → 0.16.0; compile clean, zero warnings; firmware 1645888 bytes (+192 B).

### Session 19: Battery level reporting (DevStatusAns)

- [x] HAL: `hal/esp32_board.c` now holds a `static volatile uint8_t s_battery_level = 255` (matches LoRaWAN DevStatusAns "unable to measure" default). `BoardGetBatteryLevel()` reads it; new `BoardSetBatteryLevel(uint8_t)` stores it. Volatile is enough — a single-byte store is atomic on xtensa, the MAC task only reads, the Python thread only writes, no mutex needed.
- [x] Python API: `lw.battery_level([level])` getter/setter. Validates `0..255` and raises `ValueError` out of range. Does not require `initialized` or a command round-trip — writes the static directly, so it's safe to call before join or from any thread.
- [x] `examples/sensor_node.py` — refactored: new `read_battery_mv(hw)` + `battery_level_lorawan(mv)` helpers split out of `read_sensor`. `main()` now calls `lw.battery_level(battery_level_lorawan(mv))` before `send()`, so the next `DevStatusReq` from the network gets a real answer. Prints the mapped level alongside the mV reading for visibility.
- [x] README — new `battery_level()` entry under *Configuration*, documenting the `0 / 1..254 / 255` semantics and pointing to `sensor_node.py`. Examples README annotates `sensor_node.py` with the new capability.
- [x] Version bumped 0.17.0 → 0.18.0; compile clean, firmware 0x191f40 bytes.

### Session 20: API refinement (naming consistency, error handling, callbacks)

Review of the full Python API surface identified several inconsistencies to fix before v1.0.

#### Naming

- [ ] Rename `request_class(cls)` → overload `device_class(cls)` as setter. All other getter/setters use one name (`datarate()`, `adr()`, `tx_power()`, etc.), but class uses two (`device_class()` getter + `request_class()` setter). Keep `request_class` as a deprecated alias for one release. For Class B (async), `device_class(CLASS_B)` returns immediately and `on_class_change` fires later — document this clearly.
- [ ] Rename constructor kwarg `rx2_dr` → `rx2_datarate` for consistency with the `datarate()` method name. Keep `rx2_dr` as a deprecated alias. Similarly `rx2_freq` is fine (no `frequency()` method).

#### Callbacks

- [ ] Unify callback argument passing — currently `on_rx` receives 5 separate args, `on_beacon` receives a 2-tuple, fragmentation callbacks receive tuples. Pick one convention. Recommendation: all callbacks receive separate named-position args (not tuples), matching `on_rx(data, port, rssi, snr, multicast)` pattern. Update `on_beacon(state, info)` to two separate args (currently a single 2-tuple). Update fragmentation `on_progress(counter, nb, size, lost)` and `on_done(status, size)` to separate args.
- [ ] `recv()` returns 5-tuple `(data, port, rssi, snr, multicast)` — verify this matches `on_rx` shape. The Session 14 notes say `recv()` still returns 4-tuple for backwards compat — if so, update to 5-tuple for consistency.

#### Error handling

- [ ] Standardise exception types: `OSError(errno)` for I/O and timeout conditions, `ValueError` for invalid arguments, `RuntimeError` only for state errors. Specific changes:
      - `send()` timeout: change from `RuntimeError("send timeout")` to `OSError(ETIMEDOUT)` (matches `join_otaa` timeout)
      - `request_class()` failure: change from `RuntimeError("class switch failed")` to `OSError(EIO)` or similar
      - Keep `RuntimeError` for state precondition failures (e.g. "not joined", "package not enabled")

#### Time API clarity

- [ ] Remove `network_time()` — it returns the frozen snapshot from last DeviceTimeAns, which is rarely what users want. `synced_time()` returns the live advancing time and covers all use cases. If someone needs the snapshot timestamp, they can capture it in the `on_time_sync` callback. Alternatively, rename `network_time()` → `last_sync_epoch()` if we keep it.

#### Documentation

- [ ] Add `__doc__` strings to all Python-facing methods in modlorawan.c (MP_DEFINE_CONST_FUN_OBJ accepts a doc string via the MP_ROM_xxx macros — check if MicroPython v1.29 supports this, otherwise skip)
- [ ] Verify README API reference matches the actual implementation after all renames

#### Version bump

- [ ] Bump version to 1.0.0 after API refinement — signals stable API surface

## Notes

- TCXO register: T-Beam uses crystal (0x09), NOT TCXO (0x19). Wrong value = radio doesn't work.
- EU868 RX2 DR: Must be DR_3 for TTN, not the LoRaWAN default DR_0.
- LoRa Reset: GPIO 23 across all T-Beam versions.
- GPS pins differ: v0.7 uses RX=12/TX=15, v1.0+ uses RX=34/TX=12.
- v0.7 has no PMU — battery via TP4054 charger and ADC voltage divider.
