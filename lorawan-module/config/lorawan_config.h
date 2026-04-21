#ifndef LORAWAN_CONFIG_H
#define LORAWAN_CONFIG_H

/* Active regions (each adds ~10 KB flash).
 * REGION_EU868 is also set by micropython.cmake via -DREGION_EU868 so cmake
 * controls which regions are compiled; these lines serve as documentation. */
#ifndef REGION_EU868
#define REGION_EU868
#endif
/* #define REGION_US915 */
/* #define REGION_AU915 */
/* #define REGION_AS923 */

/* T-Beam default pins — common across all variants (v0.7 through v1.2) */
#define RADIO_MOSI_PIN          27
#define RADIO_MISO_PIN          19
#define RADIO_SCLK_PIN          5
#define RADIO_NSS_PIN           18
#define RADIO_RESET_PIN         23  /* GPIO23 confirmed across all T-Beam versions */

/* SX1276-specific */
#define RADIO_DIO0_PIN          26  /* Main interrupt */
#define RADIO_DIO1_PIN_1276     33  /* RX timeout / CAD */

/* SX1262-specific */
#define RADIO_DIO1_PIN          33  /* Main interrupt */
#define RADIO_BUSY_PIN          32  /* BUSY signal (required) */

/* SPI host — uses ESP-IDF SPI3_HOST (VSPI) */
#define RADIO_SPI_SPEED         10000000  /* 10 MHz */

/* Default device class */
#define LORAWAN_DEFAULT_CLASS   CLASS_A

/* Include both radio drivers; runtime selection in modlorawan.c */
#define USE_RADIO_SX1276
#define USE_RADIO_SX1262

#endif /* LORAWAN_CONFIG_H */
