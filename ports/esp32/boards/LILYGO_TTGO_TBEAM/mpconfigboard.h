#define MICROPY_HW_BOARD_NAME "LILYGO T-Beam"
#define MICROPY_HW_MCU_NAME "ESP32"
#define MICROPY_PY_NETWORK_HOSTNAME_DEFAULT "mpy-tbeam"

// Enable BLE
#define MICROPY_PY_BLUETOOTH (1)
#define MICROPY_PY_BLUETOOTH_USE_SYNC_EVENTS (1)

// SPIRAM — present on v1.0+ (4MB PSRAM); v0.7 has none.
// sdkconfig.spiram sets CONFIG_SPIRAM_IGNORE_NOTFOUND=y so builds without PSRAM still boot.
#define MICROPY_HW_SPIRAM_SPEED (80)

// SPI1 default pins — LoRa radio (common to all T-Beam versions)
// Sources: Meshtastic variant.h, LILYGO official repos, confirmed schematics
#define MICROPY_HW_SPI1_MOSI (27)
#define MICROPY_HW_SPI1_MISO (19)
#define MICROPY_HW_SPI1_SCK  (5)

// I2C0 default pins — PMU (AXP192/AXP2101) and OLED
#define MICROPY_HW_I2C0_SCL (22)
#define MICROPY_HW_I2C0_SDA (21)
