/* Rename LoRaMAC-node AES symbols to avoid conflict with ESP-IDF wpa_supplicant.
 * Include this header before any aes.h include in soft-se sources. */
#ifndef LORAWAN_AES_RENAME_H
#define LORAWAN_AES_RENAME_H

#define aes_set_key   lorawan_aes_set_key
#define aes_encrypt   lorawan_aes_encrypt
#define aes_decrypt   lorawan_aes_decrypt

#endif /* LORAWAN_AES_RENAME_H */
