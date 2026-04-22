#ifndef RADIO_SELECT_H
#define RADIO_SELECT_H

#include <stdbool.h>

// Set the active radio driver.  Call once at startup, before any MAC
// operation.  is_sx1276=true selects the SX1276 driver, false selects SX126x.
void lorawan_radio_select(bool is_sx1276);

// Call before LoRaMacProcess() on every task iteration.
// SX126x requires polling RadioIrqProcess() to dispatch DIO1 events;
// SX1276 drives events from ISR directly so IrqProcess is NULL (no-op).
void lorawan_radio_irq_process(void);

#endif /* RADIO_SELECT_H */
