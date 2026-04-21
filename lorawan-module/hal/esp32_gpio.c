#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gpio.h"

void GpioInit(Gpio_t *obj, PinNames pin, PinModes mode, PinConfigs config,
              PinTypes type, uint32_t value)
{
    if (obj) {
        obj->pin = pin;
    }
}

void GpioSetContext(Gpio_t *obj, void *context)
{
    if (obj) {
        obj->Context = context;
    }
}

void GpioSetInterrupt(Gpio_t *obj, IrqModes irqMode, IrqPriorities irqPriority,
                      GpioIrqHandler *irqHandler)
{
    if (obj) {
        obj->IrqHandler = irqHandler;
    }
}

void GpioRemoveInterrupt(Gpio_t *obj)
{
    if (obj) {
        obj->IrqHandler = NULL;
    }
}

void GpioWrite(Gpio_t *obj, uint32_t value)
{
    (void)obj;
    (void)value;
}

void GpioToggle(Gpio_t *obj)
{
    (void)obj;
}

uint32_t GpioRead(Gpio_t *obj)
{
    (void)obj;
    return 0;
}
