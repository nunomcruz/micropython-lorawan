#include <stdint.h>
#include "spi.h"

void SpiInit(Spi_t *obj, SpiId_t spiId, PinNames mosi, PinNames miso,
             PinNames sclk, PinNames nss)
{
    if (obj) {
        obj->SpiId = spiId;
    }
}

void SpiDeInit(Spi_t *obj)
{
    (void)obj;
}

void SpiFormat(Spi_t *obj, int8_t bits, int8_t cpol, int8_t cpha, int8_t slave)
{
    (void)obj; (void)bits; (void)cpol; (void)cpha; (void)slave;
}

void SpiFrequency(Spi_t *obj, uint32_t hz)
{
    (void)obj; (void)hz;
}

uint16_t SpiInOut(Spi_t *obj, uint16_t outData)
{
    (void)obj; (void)outData;
    return 0;
}
