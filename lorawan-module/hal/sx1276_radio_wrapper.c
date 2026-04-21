// Namespace wrapper for sx1276.c.
// Renames the three global symbols that collide with sx126x/radio.c so both
// radio drivers can be linked into the same binary.  The originals in
// loramac-node/ are NOT modified.
#define FskBandwidths   SX1276_FskBandwidths
#define RxTimeoutTimer  SX1276_RxTimeoutTimer
#define TxTimeoutTimer  SX1276_TxTimeoutTimer

#include "../loramac-node/src/radio/sx1276/sx1276.c"
