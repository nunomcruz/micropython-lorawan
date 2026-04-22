#include "pin_config.h"
#include "lorawan_config.h"
#include "driver/spi_master.h"

volatile int8_t g_tx_power_hw_override = LORAWAN_TX_POWER_NO_OVERRIDE;

lorawan_pins_t g_lorawan_pins = {
    .spi_host   = SPI3_HOST,           /* VSPI — T-Beam default */
    .mosi       = RADIO_MOSI_PIN,       /* 27 */
    .miso       = RADIO_MISO_PIN,       /* 19 */
    .sclk       = RADIO_SCLK_PIN,       /* 5  */
    .nss        = RADIO_NSS_PIN,        /* 18 */
    .reset      = RADIO_RESET_PIN,      /* 23 */
    .dio0       = RADIO_DIO0_PIN,       /* 26 */
    .dio1_1276  = RADIO_DIO1_PIN_1276,  /* 33 */
    .dio1_1262  = RADIO_DIO1_PIN,       /* 33 */
    .busy       = RADIO_BUSY_PIN,       /* 32 */
};
