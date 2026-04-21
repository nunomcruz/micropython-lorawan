#include <string.h>
#include <stdbool.h>

// Import Radio_s struct definition without triggering the "extern const Radio"
// declaration that radio.h carries (which would conflict with our non-const def).
#define Radio __lorawan_radio_extern_decl
#include "radio.h"
#undef Radio

#include "radio_select.h"

// Radio function-pointer tables — defined in the namespace wrapper TUs.
extern const struct Radio_s Radio_SX1276;   // hal/sx1276_radio_wrapper.c
extern const struct Radio_s Radio_SX126x;   // hal/sx126x_radio_wrapper.c

// The Radio symbol used by LoRaMAC-node.  Declared as "extern const" in
// radio.h to protect other TUs from writing it; this definition is the
// authoritative, writable copy.
struct Radio_s Radio;

void lorawan_radio_select(bool is_sx1276) {
    if (is_sx1276) {
        Radio = Radio_SX1276;
    } else {
        Radio = Radio_SX126x;
    }
}
