#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "gpio.h"

#include "driver/gpio.h"
#include "esp_attr.h"

static bool s_isr_service_installed = false;

static void ensure_isr_service(void) {
    if (!s_isr_service_installed) {
        gpio_install_isr_service(0);
        s_isr_service_installed = true;
    }
}

static IRAM_ATTR void gpio_isr_trampoline(void *arg) {
    Gpio_t *obj = (Gpio_t *)arg;
    if (obj && obj->IrqHandler) {
        obj->IrqHandler(obj->Context);
    }
}

void GpioInit(Gpio_t *obj, PinNames pin, PinModes mode, PinConfigs config,
              PinTypes type, uint32_t value) {
    if (!obj) return;

    obj->pin = pin;
    obj->IrqHandler = NULL;
    obj->Context = NULL;

    if (pin == NC) return;

    gpio_num_t gpio = (gpio_num_t)pin;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_up_en   = (type == PIN_PULL_UP)   ? GPIO_PULLUP_ENABLE   : GPIO_PULLUP_DISABLE,
        .pull_down_en = (type == PIN_PULL_DOWN)  ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
    };

    if (mode == PIN_OUTPUT) {
        cfg.mode = (config == PIN_OPEN_DRAIN) ? GPIO_MODE_OUTPUT_OD : GPIO_MODE_OUTPUT;
    } else {
        cfg.mode = GPIO_MODE_INPUT;
    }

    gpio_config(&cfg);

    if (mode == PIN_OUTPUT) {
        gpio_set_level(gpio, value ? 1 : 0);
    }
}

void GpioSetContext(Gpio_t *obj, void *context) {
    if (obj) obj->Context = context;
}

void GpioSetInterrupt(Gpio_t *obj, IrqModes irqMode, IrqPriorities irqPriority,
                      GpioIrqHandler *irqHandler) {
    if (!obj || obj->pin == NC) return;

    obj->IrqHandler = irqHandler;
    ensure_isr_service();

    gpio_num_t gpio = (gpio_num_t)obj->pin;

    gpio_int_type_t intr_type;
    switch (irqMode) {
        case IRQ_RISING_EDGE:         intr_type = GPIO_INTR_POSEDGE; break;
        case IRQ_FALLING_EDGE:        intr_type = GPIO_INTR_NEGEDGE; break;
        case IRQ_RISING_FALLING_EDGE: intr_type = GPIO_INTR_ANYEDGE; break;
        default:                      intr_type = GPIO_INTR_DISABLE; break;
    }

    gpio_set_intr_type(gpio, intr_type);
    gpio_isr_handler_add(gpio, gpio_isr_trampoline, (void *)obj);
    gpio_intr_enable(gpio);
}

void GpioRemoveInterrupt(Gpio_t *obj) {
    if (!obj || obj->pin == NC) return;
    gpio_intr_disable((gpio_num_t)obj->pin);
    gpio_isr_handler_remove((gpio_num_t)obj->pin);
    gpio_set_intr_type((gpio_num_t)obj->pin, GPIO_INTR_DISABLE);
    obj->IrqHandler = NULL;
}

void IRAM_ATTR GpioWrite(Gpio_t *obj, uint32_t value) {
    if (!obj || obj->pin == NC) return;
    gpio_set_level((gpio_num_t)obj->pin, value ? 1 : 0);
}

void IRAM_ATTR GpioToggle(Gpio_t *obj) {
    if (!obj || obj->pin == NC) return;
    gpio_num_t gpio = (gpio_num_t)obj->pin;
    gpio_set_level(gpio, !gpio_get_level(gpio));
}

uint32_t IRAM_ATTR GpioRead(Gpio_t *obj) {
    if (!obj || obj->pin == NC) return 0;
    return (uint32_t)gpio_get_level((gpio_num_t)obj->pin);
}
