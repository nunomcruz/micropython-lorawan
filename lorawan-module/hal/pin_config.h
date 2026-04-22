#ifndef LORAWAN_PIN_CONFIG_H
#define LORAWAN_PIN_CONFIG_H

/* Runtime pin configuration for the LoRaWAN radio.
 * Defaults are T-Beam values (lorawan_config.h).
 * Set by lorawan.LoRaWAN() kwargs before any IoInit call. */

typedef struct {
    int spi_host;       /* ESP-IDF SPI host: SPI2_HOST=1 (HSPI), SPI3_HOST=2 (VSPI) */
    int mosi;
    int miso;
    int sclk;
    int nss;            /* chip select */
    int reset;
    int dio0;           /* SX1276: main IRQ (TX/RX done) */
    int dio1_1276;      /* SX1276: secondary IRQ (RX timeout / CAD) */
    int dio1_1262;      /* SX1262: main IRQ */
    int busy;           /* SX1262: BUSY */
} lorawan_pins_t;

extern lorawan_pins_t g_lorawan_pins;

/* Hardware TX power override in dBm.
 * When != INT8_MIN, SX*SetRfTxPower() uses this value regardless of
 * what the MAC stack requests. Set when the user requests power beyond
 * the region's regulatory limit. ADR must be disabled while active.
 * Hardware caps apply: SX1276 ≤ 20 dBm, SX1262 ≤ 22 dBm. */
#include <stdint.h>
#define LORAWAN_TX_POWER_NO_OVERRIDE  INT8_MIN
extern volatile int8_t g_tx_power_hw_override;

#endif /* LORAWAN_PIN_CONFIG_H */
