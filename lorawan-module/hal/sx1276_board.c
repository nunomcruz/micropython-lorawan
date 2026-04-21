#include <stdint.h>
#include <stdbool.h>
#include "sx1276/sx1276.h"
#include "sx1276-board.h"

void SX1276IoInit(void)
{
}

void SX1276IoIrqInit(DioIrqHandler **irqHandlers)
{
    (void)irqHandlers;
}

void SX1276IoDeInit(void)
{
}

void SX1276IoTcxoInit(void)
{
}

void SX1276IoDbgInit(void)
{
}

void SX1276Reset(void)
{
}

void SX1276SetRfTxPower(int8_t power)
{
    (void)power;
}

void SX1276SetAntSwLowPower(bool status)
{
    (void)status;
}

void SX1276AntSwInit(void)
{
}

void SX1276AntSwDeInit(void)
{
}

void SX1276SetAntSw(uint8_t opMode)
{
    (void)opMode;
}

bool SX1276CheckRfFrequency(uint32_t frequency)
{
    (void)frequency;
    return true;
}

void SX1276SetBoardTcxo(uint8_t state)
{
    (void)state;
}

uint32_t SX1276GetBoardTcxoWakeupTime(void)
{
    return 0;
}

uint32_t SX1276GetDio1PinState(void)
{
    return 0;
}

void SX1276DbgPinTxWrite(uint8_t state)
{
    (void)state;
}

void SX1276DbgPinRxWrite(uint8_t state)
{
    (void)state;
}
