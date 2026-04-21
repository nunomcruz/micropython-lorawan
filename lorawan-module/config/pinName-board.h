/* ESP32 GPIO pin names for LoRaMAC-node */
#ifndef __PIN_NAME_BOARD_H__
#define __PIN_NAME_BOARD_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ESP32 has GPIO 0–39; use simple integer mapping */
#define MCU_PINS \
    GPIO_0 = 0,  GPIO_1,  GPIO_2,  GPIO_3,  GPIO_4,  GPIO_5,  \
    GPIO_6,      GPIO_7,  GPIO_8,  GPIO_9,  GPIO_10, GPIO_11, \
    GPIO_12,     GPIO_13, GPIO_14, GPIO_15, GPIO_16, GPIO_17, \
    GPIO_18,     GPIO_19, GPIO_20, GPIO_21, GPIO_22, GPIO_23, \
    GPIO_24,     GPIO_25, GPIO_26, GPIO_27, GPIO_28, GPIO_29, \
    GPIO_30,     GPIO_31, GPIO_32, GPIO_33, GPIO_34, GPIO_35, \
    GPIO_36,     GPIO_37, GPIO_38, GPIO_39

#ifdef __cplusplus
}
#endif

#endif /* __PIN_NAME_BOARD_H__ */
