#include <string.h>
#include <stdbool.h>

// Import Radio_s struct definition without triggering the "extern const Radio"
// declaration that radio.h carries (which would conflict with our non-const def).
#define Radio __lorawan_radio_extern_decl
#include "radio.h"
#undef Radio

#include "radio_select.h"

// SX1276 Radio function-pointer table — defined in hal/sx1276_board.c.
extern const struct Radio_s Radio_SX1276;

// SX126x Radio function-pointer table (Phase 4).
// sx126x/radio.c shares global symbol names with sx1276.c and cannot be
// linked in the same binary without namespace isolation.  Phase 4 will add
// a proper SX126x Radio table once the MAC layer is wired up.
static const struct Radio_s s_radio_sx126x_placeholder;  // all-zero / NULL ptrs

// The Radio symbol used by LoRaMAC-node.  Declared as "extern const" in
// radio.h to protect other TUs from writing it; this definition is the
// authoritative, writable copy.
struct Radio_s Radio;

void lorawan_radio_select(bool is_sx1276) {
    if (is_sx1276) {
        Radio = Radio_SX1276;
    } else {
        // Phase 4: replace with real SX126x Radio table
        Radio = s_radio_sx126x_placeholder;
    }
}
