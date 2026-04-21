add_library(usermod_lorawan INTERFACE)

# Compiler flags for LoRaMAC-node
target_compile_definitions(usermod_lorawan INTERFACE
    REGION_EU868        # passes -DREGION_EU868 so Region.c includes RegionEU868.h
    SOFT_SE             # enables KeyList and NUM_OF_KEYS in secure-element-nvm.h
    # REGION_US915
    # REGION_AU915
)

# MicroPython binding
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/bindings/modlorawan.c
)

# ESP32 HAL (implemented in Phase 3, Sessions 4–6)
target_sources(usermod_lorawan INTERFACE
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
)

# Radio core drivers (both included for HAL; runtime selection via Radio ptr).
# sx126x/radio.c (the Radio_s function-pointer table for SX126x) is NOT compiled
# here — it shares global names (TxTimeoutTimer, FskBandwidths, …) with sx1276.c
# and LoRaMAC-node was not designed to link both simultaneously.  The SX126x
# Radio table will be wired up in Phase 4 when MAC integration begins.
target_sources(usermod_lorawan INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/radio/sx1276/sx1276.c
    ${CMAKE_CURRENT_LIST_DIR}/loramac-node/src/radio/sx126x/sx126x.c
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
)

target_link_libraries(usermod INTERFACE usermod_lorawan)
