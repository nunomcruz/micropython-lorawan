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

### Session 6: Integration and Test

- [ ] Implement runtime radio selection via Radio function pointer table (Radio_SX1276 vs Radio_SX126x)
- [ ] Compile full HAL — resolve any symbol conflicts with LoRaMAC-node
- [ ] Test SPI: read register 0x42 on SX1276 board, must return 0x12
- [ ] Test timer accuracy: 1s timer callback, verify < 5ms jitter
- [ ] Test on SX1262 board if available: BUSY pin reads, command interface

## Phase 4 — Python Bindings (Sessions 7–10)

### Session 7: ABP Join + Send (first uplink!)

- [ ] Implement lorawan_obj_t struct in modlorawan.c
- [ ] Implement LoRaWAN.__init__ (radio detection, MAC init, pin config)
- [ ] Implement join_abp() — configure DevAddr, NwkSKey, AppSKey
- [ ] Implement send() — uplink to TTN
- [ ] Create dedicated FreeRTOS task for LoRaWAN processing
- [ ] **CRITICAL**: Set TCXO register to 0x09 (crystal, not TCXO) for T-Beam
- [ ] **CRITICAL**: Set EU868_RX_WND_2_DR to DR_3 for TTN
- [ ] Test: ABP uplink appears on TTN console with correct payload

### Session 8: OTAA Join

- [ ] Implement join_otaa() — full join procedure with dev_eui, join_eui, app_key
- [ ] Implement joined() — returns bool
- [ ] Implement stats() — returns dict with rssi, snr, frame counters
- [ ] Handle join timeout (blocking with timeout parameter)
- [ ] Test: successful OTAA join on TTN, device shows as joined

### Session 9: Receive + Confirmed Messages

- [ ] Implement recv() — blocking downlink with timeout
- [ ] Implement confirmed uplink (confirmed=True in send())
- [ ] Implement on_rx() callback — use mp_sched_schedule() to bridge to Python
- [ ] Implement on_tx_done() callback
- [ ] Test: receive scheduled downlink from TTN
- [ ] Test: confirmed uplink with ACK

### Session 10: Persistence + ADR

- [ ] Implement nvram_save() — store session to ESP32 NVS
- [ ] Implement nvram_restore() — restore session from NVS
- [ ] Implement datarate(), adr(), tx_power() getters/setters
- [ ] Test: reboot without re-join, frame counter continues
- [ ] Test: ADR adjusts data rate after several uplinks

## Phase 5 — Advanced Features (Sessions 11–15)

### Session 11: Core extras

- [ ] Full runtime pin configuration from Python (for non-T-Beam boards)
- [ ] End-to-end SX1262 test: join, uplink, downlink, confirmed, persistence
- [ ] Class C support (`request_class(CLASS_C)`, continuous RX2 window)
- [ ] Implement `on_class_change(callback)` — confirmed class transitions
- [ ] EU433 region support (enable REGION_EU433 in config)

### Session 12: Advanced MAC commands + time sync

- [ ] Implement `request_device_time()` — MLME_DEVICE_TIME, sends DeviceTimeReq
- [ ] Implement `network_time()` — returns GPS epoch seconds from last DeviceTimeAns
- [ ] Implement `on_time_sync(callback)` — notification when time is received
- [ ] Implement `link_check()` — MLME_LINK_CHECK, returns margin + gateway count
- [ ] Implement `rejoin(type=0|1|2)` — LoRaWAN 1.1 rejoin without full join
- [ ] Register Clock Sync package (LmhpClockSync, port 202)
- [ ] Implement `clock_sync_enable()`, `clock_sync_request()`, `synced_time()`
- [ ] Test: DeviceTimeReq on TTN, verify epoch matches real time
- [ ] Test: LinkCheckReq, verify gateway count and margin
- [ ] Test: Clock sync time correction

### Session 13: Class B (beacons + ping slots)

Prerequisites: DeviceTimeReq must work (Session 12), timer HAL accuracy verified (Session 6).

- [ ] Implement HAL `GetTemperatureLevel()` — read from AXP PMU or fixed estimate (for beacon drift compensation)
- [ ] Implement `request_class(CLASS_B)` — async transition: beacon acquisition → PingSlotInfoReq → confirmation
- [ ] Implement `ping_slot_periodicity(N)` — 2^N seconds between ping slots (N=0..7)
- [ ] Implement `on_beacon(callback)` — states: LOCKED, LOST, ACQUISITION, TIMEOUT, REACQUISITION
- [ ] Implement `beacon_state()` — returns dict with beacon time, gateway info, etc.
- [ ] Implement `device_class()` — returns current class (A, B, or C)
- [ ] Handle `MLME_REVERT_JOIN` — automatic revert to Class A after MAX_BEACON_LESS_PERIOD (2h)
- [ ] Test: DeviceTimeReq → request Class B → beacon locked → receive on ping slot
- [ ] Test: beacon loss → reacquisition → automatic revert after timeout

### Session 14: Multicast (local + remote, Class B + C)

- [ ] Implement `multicast_add(group, addr, mc_nwk_s_key, mc_app_s_key, ...)` — local setup, up to 4 groups
- [ ] Implement `multicast_rx_params(group, device_class, frequency, datarate, ...)` — Class B (with periodicity) or Class C
- [ ] Implement `multicast_remove(group)` — delete group
- [ ] Implement `multicast_list()` — returns active groups with metadata
- [ ] Ensure `on_rx(callback)` receives multicast downlinks with group info in metadata
- [ ] Register Remote Multicast Setup package (LmhpRemoteMcastSetup) for server-driven configuration
- [ ] Implement `remote_multicast_enable()` — server can then send McGroupSetupReq
- [ ] Register Fragmentation package (LmhpFragmentation) with progress/done callbacks
- [ ] Implement `fragmentation_enable(buffer_size, on_progress, on_done)` — for FUOTA
- [ ] Test: local multicast Class C — device receives on multicast address
- [ ] Test: local multicast Class B — device receives on multicast ping slot
- [ ] Test: remote multicast setup via LmhpRemoteMcastSetup (if server supports it)

### Session 15: Documentation and examples

- [ ] API reference documentation
- [ ] Hardware setup guide
- [ ] Example scripts: basic_otaa.py, basic_abp.py, sensor_node.py
- [ ] Example scripts: time_sync.py, class_b_beacon.py, multicast_receiver.py

## Notes

- TCXO register: T-Beam uses crystal (0x09), NOT TCXO (0x19). Wrong value = radio doesn't work.
- EU868 RX2 DR: Must be DR_3 for TTN, not the LoRaWAN default DR_0.
- LoRa Reset: GPIO 23 across all T-Beam versions.
- GPS pins differ: v0.7 uses RX=12/TX=15, v1.0+ uses RX=34/TX=12.
- v0.7 has no PMU — battery via TP4054 charger and ADC voltage divider.
