#include <stdint.h>
#include <stdbool.h>

#include "gpio.h"
#include "spi.h"
#include "delay.h"
#include "lorawan_config.h"

#include "sx1276/sx1276.h"
#include "sx1276-board.h"
#include "radio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void SX1276IoInit(void)
{
    // NSS: output, start high (deasserted)
    GpioInit(&SX1276.Spi.Nss, (PinNames)RADIO_NSS_PIN,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1);

    // Reset: output, start high
    GpioInit(&SX1276.Reset, (PinNames)RADIO_RESET_PIN,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1);

    // DIO0: input, no pull — main interrupt for TX/RX done
    GpioInit(&SX1276.DIO0, (PinNames)RADIO_DIO0_PIN,
             PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);

    // DIO1: input, no pull — RX timeout / CAD done
    GpioInit(&SX1276.DIO1, (PinNames)RADIO_DIO1_PIN_1276,
             PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);

    // Initialise SPI bus (MOSI=27, MISO=19, SCLK=5, NSS managed manually)
    SpiInit(&SX1276.Spi, SPI_1,
            (PinNames)RADIO_MOSI_PIN, (PinNames)RADIO_MISO_PIN,
            (PinNames)RADIO_SCLK_PIN, NC);
}

void SX1276IoIrqInit(DioIrqHandler **irqHandlers)
{
    if (!irqHandlers) return;

    // DIO0 → irqHandlers[0] (TX done / RX done / CRC error)
    if (irqHandlers[0]) {
        GpioSetInterrupt(&SX1276.DIO0, IRQ_RISING_EDGE,
                         IRQ_HIGH_PRIORITY, irqHandlers[0]);
    }
    // DIO1 → irqHandlers[1] (RX timeout / CAD done / FHSS change channel)
    if (irqHandlers[1]) {
        GpioSetInterrupt(&SX1276.DIO1, IRQ_RISING_EDGE,
                         IRQ_HIGH_PRIORITY, irqHandlers[1]);
    }
}

void SX1276IoDeInit(void)
{
    GpioRemoveInterrupt(&SX1276.DIO0);
    GpioRemoveInterrupt(&SX1276.DIO1);
    SpiDeInit(&SX1276.Spi);
    GpioInit(&SX1276.Spi.Nss, (PinNames)RADIO_NSS_PIN,
             PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);
}

void SX1276IoTcxoInit(void)
{
    // T-Beam SX1276 uses a crystal — no TCXO control pin needed.
    // REG_LR_TCXO must be set to 0x09 (crystal) not 0x19 (TCXO).
    // This is handled in SX1276Init via the register init table.
}

void SX1276IoDbgInit(void)
{
    // No debug pins on T-Beam.
}

void SX1276Reset(void)
{
    // Drive reset low for 10 ms then release; SX1276 datasheet §7.2.2
    GpioInit(&SX1276.Reset, (PinNames)RADIO_RESET_PIN,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);
    DelayMs(10);
    GpioInit(&SX1276.Reset, (PinNames)RADIO_RESET_PIN,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1);
    DelayMs(6);
}

void SX1276SetRfTxPower(int8_t power)
{
    uint8_t pa_config = SX1276Read(REG_LR_PACONFIG);
    uint8_t pa_dac    = SX1276Read(REG_LR_PADAC);

    // T-Beam always uses PA_BOOST path
    pa_config = (pa_config & RF_PACONFIG_PASELECT_MASK) |
                RF_PACONFIG_PASELECT_PABOOST;
    pa_config = (pa_config & RF_PACONFIG_MAX_POWER_MASK) | 0x70;

    if (power > 17) {
        // +20 dBm mode via PA_DAC
        pa_dac = (pa_dac & RF_PADAC_20DBM_MASK) | RF_PADAC_20DBM_ON;
        if (power < 5)  power = 5;
        if (power > 20) power = 20;
        pa_config = (pa_config & RF_PACONFIG_OUTPUTPOWER_MASK) |
                    (uint8_t)((uint16_t)(power - 5) & 0x0F);
    } else {
        pa_dac = (pa_dac & RF_PADAC_20DBM_MASK) | RF_PADAC_20DBM_OFF;
        if (power < 2)  power = 2;
        if (power > 17) power = 17;
        pa_config = (pa_config & RF_PACONFIG_OUTPUTPOWER_MASK) |
                    (uint8_t)((uint16_t)(power - 2) & 0x0F);
    }

    SX1276Write(REG_LR_PACONFIG, pa_config);
    SX1276Write(REG_LR_PADAC, pa_dac);
}

void SX1276SetAntSwLowPower(bool status)
{
    (void)status;
    // T-Beam has no software-controlled RF switch on the SX1276 path.
}

void SX1276AntSwInit(void)   {}
void SX1276AntSwDeInit(void) {}

void SX1276SetAntSw(uint8_t opMode)
{
    (void)opMode;
    // T-Beam SX1276 board has a passive switch; no GPIO control required.
}

bool SX1276CheckRfFrequency(uint32_t frequency)
{
    // EU868 band: 863–870 MHz; accept full ISM range 150 MHz–960 MHz
    return (frequency >= 150000000UL && frequency <= 960000000UL);
}

void SX1276SetBoardTcxo(uint8_t state)
{
    (void)state;
    // T-Beam SX1276 uses a crystal, not a TCXO. No action required.
}

uint32_t SX1276GetBoardTcxoWakeupTime(void)
{
    return 0;
}

uint32_t SX1276GetDio1PinState(void)
{
    return GpioRead(&SX1276.DIO1);
}

void SX1276DbgPinTxWrite(uint8_t state) { (void)state; }
void SX1276DbgPinRxWrite(uint8_t state) { (void)state; }

// Radio function-pointer table for LoRaMAC-node.  Named Radio_SX1276 so the
// linker-visible Radio symbol remains in hal/radio_select.c (writable, needed
// for runtime radio selection).
const struct Radio_s Radio_SX1276 = {
    SX1276Init,
    SX1276GetStatus,
    SX1276SetModem,
    SX1276SetChannel,
    SX1276IsChannelFree,
    SX1276Random,
    SX1276SetRxConfig,
    SX1276SetTxConfig,
    SX1276CheckRfFrequency,       // board-supplied: hal/sx1276_board.c
    SX1276GetTimeOnAir,
    SX1276Send,
    SX1276SetSleep,
    SX1276SetStby,
    SX1276SetRx,
    SX1276StartCad,
    SX1276SetTxContinuousWave,
    SX1276ReadRssi,
    SX1276Write,
    SX1276Read,
    SX1276WriteBuffer,
    SX1276ReadBuffer,
    SX1276SetMaxPayloadLength,
    SX1276SetPublicNetwork,
    SX1276GetWakeupTime,
    NULL,  // IrqProcess — SX1276 uses hardware DIO interrupts, no polling
    NULL,  // RxBoosted  — SX126x only
    NULL,  // SetRxDutyCycle — SX126x only
};
