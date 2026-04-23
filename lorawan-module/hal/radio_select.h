#ifndef RADIO_SELECT_H
#define RADIO_SELECT_H

#include <stdbool.h>
#include <stdint.h>

// Set the active radio driver.  Call once at startup, before any MAC
// operation.  is_sx1276=true selects the SX1276 driver, false selects SX126x.
void lorawan_radio_select(bool is_sx1276);

// Call before LoRaMacProcess() on every task iteration.
// SX126x requires polling RadioIrqProcess() to dispatch DIO1 events;
// SX1276 drives events from ISR directly so IrqProcess is NULL (no-op).
void lorawan_radio_irq_process(void);

// Bring up the SX1276 pin/SPI HAL, reset the chip, and read the version
// register (0x42).  Returns 0x12 if an SX1276 is present, something else
// (typically 0x00 on the SX1262 which returns NACK for this SPI opcode)
// otherwise.  This is how we auto-detect the radio at boot.
//
// Wrapped here so callers don't have to include sx1276-board.h /
// sx1276/sx1276.h — which clash with sx126x.h on a handful of
// register/syncword macros that differ between the two chips.
uint8_t lorawan_radio_probe_reg42(void);

// Bring up the SX126x pin/SPI HAL.  Must be called once before any MAC
// operation when the detected radio is an SX126x.  Safe no-op if the
// SX126x is not present (it will not respond; failures surface later
// during MAC init).
void lorawan_radio_init_sx126x(void);

#endif /* RADIO_SELECT_H */
