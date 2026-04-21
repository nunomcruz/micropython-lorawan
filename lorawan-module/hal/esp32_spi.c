#include <stdint.h>
#include <string.h>

#include "spi.h"
#include "lorawan_config.h"

#include "driver/spi_master.h"
#include "esp_attr.h"

// SX1276 and SX126x manage NSS (chip select) manually via GpioWrite, so we
// use software CS (spics_io_num = -1) and a bare SPI bus with no hardware CS.
// The device handle is stashed in obj->Nss.port (void *) to avoid adding
// fields to the upstream Spi_t struct.

#define SPI_DEV_HANDLE(obj)  ((spi_device_handle_t)((obj)->Nss.port))

void SpiInit(Spi_t *obj, SpiId_t spiId, PinNames mosi, PinNames miso,
             PinNames sclk, PinNames nss) {
    if (!obj) return;

    obj->SpiId    = spiId;
    obj->Nss.port = NULL;

    spi_bus_config_t buscfg = {
        .mosi_io_num     = (int)mosi,
        .miso_io_num     = (int)miso,
        .sclk_io_num     = (int)sclk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 256,
    };

    // SPI3_HOST == VSPI on ESP32; DMA auto-selects best channel
    esp_err_t err = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE means the bus is already initialised (e.g. shared)
        return;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = RADIO_SPI_SPEED,
        .mode           = 0,    // CPOL=0, CPHA=0
        .spics_io_num   = -1,   // software CS via GpioWrite
        .queue_size     = 1,
        .flags          = 0,
    };

    spi_device_handle_t handle;
    if (spi_bus_add_device(SPI3_HOST, &devcfg, &handle) == ESP_OK) {
        obj->Nss.port = (void *)handle;
    }
}

void SpiDeInit(Spi_t *obj) {
    if (!obj || !obj->Nss.port) return;
    spi_bus_remove_device(SPI_DEV_HANDLE(obj));
    spi_bus_free(SPI3_HOST);
    obj->Nss.port = NULL;
}

void SpiFormat(Spi_t *obj, int8_t bits, int8_t cpol, int8_t cpha, int8_t slave) {
    // Fixed at SpiInit: 8-bit, Mode 0, master.
    (void)obj; (void)bits; (void)cpol; (void)cpha; (void)slave;
}

void SpiFrequency(Spi_t *obj, uint32_t hz) {
    // Fixed at SpiInit: RADIO_SPI_SPEED.
    (void)obj; (void)hz;
}

// Full-duplex single-byte transfer.  The radio drivers hold CS low around
// a burst of SpiInOut calls using GpioWrite on the NSS pin.
IRAM_ATTR uint16_t SpiInOut(Spi_t *obj, uint16_t outData) {
    if (!obj || !obj->Nss.port) return 0;

    uint8_t tx = (uint8_t)outData;
    uint8_t rx = 0;

    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length    = 8;       // bits
    t.tx_buffer = &tx;
    t.rx_buffer = &rx;

    // polling_transmit avoids the overhead of a semaphore acquire on every byte
    spi_device_polling_transmit(SPI_DEV_HANDLE(obj), &t);

    return (uint16_t)rx;
}
