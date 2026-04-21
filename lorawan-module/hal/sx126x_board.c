#include <stdint.h>
#include <stdbool.h>
#include "sx126x/sx126x.h"
#include "sx126x-board.h"

static RadioOperatingModes_t operating_mode = MODE_STDBY_RC;

void SX126xIoInit(void)
{
}

void SX126xIoIrqInit(DioIrqHandler dioIrq)
{
    (void)dioIrq;
}

void SX126xIoDeInit(void)
{
}

void SX126xIoTcxoInit(void)
{
}

void SX126xIoRfSwitchInit(void)
{
}

void SX126xIoDbgInit(void)
{
}

void SX126xReset(void)
{
}

void SX126xWaitOnBusy(void)
{
}

void SX126xWakeup(void)
{
}

void SX126xWriteCommand(RadioCommands_t opcode, uint8_t *buffer, uint16_t size)
{
    (void)opcode; (void)buffer; (void)size;
}

uint8_t SX126xReadCommand(RadioCommands_t opcode, uint8_t *buffer, uint16_t size)
{
    (void)opcode; (void)buffer; (void)size;
    return 0;
}

void SX126xWriteRegister(uint16_t address, uint8_t value)
{
    (void)address; (void)value;
}

uint8_t SX126xReadRegister(uint16_t address)
{
    (void)address;
    return 0;
}

void SX126xWriteRegisters(uint16_t address, uint8_t *buffer, uint16_t size)
{
    (void)address; (void)buffer; (void)size;
}

void SX126xReadRegisters(uint16_t address, uint8_t *buffer, uint16_t size)
{
    (void)address; (void)buffer; (void)size;
}

void SX126xWriteBuffer(uint8_t offset, uint8_t *buffer, uint8_t size)
{
    (void)offset; (void)buffer; (void)size;
}

void SX126xReadBuffer(uint8_t offset, uint8_t *buffer, uint8_t size)
{
    (void)offset; (void)buffer; (void)size;
}

void SX126xSetRfTxPower(int8_t power)
{
    (void)power;
}

uint8_t SX126xGetDeviceId(void)
{
    return SX1262;
}

void SX126xAntSwOn(void)
{
}

void SX126xAntSwOff(void)
{
}

bool SX126xCheckRfFrequency(uint32_t frequency)
{
    (void)frequency;
    return true;
}

uint32_t SX126xGetBoardTcxoWakeupTime(void)
{
    return 0;
}

uint32_t SX126xGetDio1PinState(void)
{
    return 0;
}

RadioOperatingModes_t SX126xGetOperatingMode(void)
{
    return operating_mode;
}

void SX126xSetOperatingMode(RadioOperatingModes_t mode)
{
    operating_mode = mode;
}
