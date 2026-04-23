add_library(usermod_lorawan INTERFACE)

# Compiler flags for LoRaMAC-node
target_compile_definitions(usermod_lorawan INTERFACE
    REGION_EU868        # passes -DREGION_EU868 so Region.c includes RegionEU868.h
    REGION_EU433        # 433 MHz band (amateur, ISM)
    SOFT_SE             # enables KeyList and NUM_OF_KEYS in secure-element-nvm.h
    LORAMAC_CLASSB_ENABLED  # compile the beacon + ping-slot state machine
    # REGION_US915
    # REGION_AU915
)

# MicroPython binding + LmHandler package hosting shim
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/bindings/modlorawan.c
    ${CMAKE_CURRENT_LIST_DIR}/bindings/lmhandler_shim.c
)

# LmHandler application-layer packages (hosted by the shim, not the full LmHandler)
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/apps/LoRaMac/common/LmHandler/packages/LmhpClockSync.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/apps/LoRaMac/common/LmHandler/packages/LmhpRemoteMcastSetup.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/apps/LoRaMac/common/LmHandler/packages/LmhpFragmentation.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/apps/LoRaMac/common/LmHandler/packages/FragDecoder.c
)

# ESP32 HAL (implemented in Phase 3, Sessions 4–6)
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/hal/pin_config.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/esp32_gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/esp32_spi.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/esp32_timer.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/esp32_delay.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/esp32_board.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/sx1276_board.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/sx126x_board.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/radio_select.c
)

# LoRaMAC-node utility and system functions
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/boards/mcu/utilities.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/system/systime.c
)

# LoRaMAC-node MAC layer
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMac.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacAdr.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacClassB.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacCommands.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacConfirmQueue.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacCrypto.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacParser.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/LoRaMacSerializer.c
    # Regions
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/region/Region.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/region/RegionCommon.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/region/RegionEU868.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/region/RegionEU433.c
)

# Radio core drivers — both included via namespace wrappers.
# sx1276.c and sx126x/radio.c share global names (TxTimeoutTimer,
# RxTimeoutTimer, FskBandwidths, Radio).  The wrapper files rename those
# symbols before including the originals so both can link in one binary.
# Originals in loramac-node/ are NOT modified.
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/hal/sx1276_radio_wrapper.c
    ${CMAKE_CURRENT_LIST_DIR}/hal/sx126x_radio_wrapper.c
)

# Soft secure element (AES-128, CMAC, key management)
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/peripherals/soft-se/aes.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/peripherals/soft-se/cmac.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/peripherals/soft-se/soft-se.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/peripherals/soft-se/soft-se-hal.c
)

# Include paths — config/ first so our pinName-board.h overrides the STM32 one
target_include_directories(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${CMAKE_CURRENT_LIST_DIR}/hal
    ${CMAKE_CURRENT_LIST_DIR}/bindings
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/mac/region
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/radio
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/radio/sx1276
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/radio/sx126x
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/peripherals/soft-se
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/boards
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/system
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/apps/LoRaMac/common/LmHandler
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/apps/LoRaMac/common/LmHandler/packages
)

target_link_libraries(usermod INTERFACE usermod_lorawan)
