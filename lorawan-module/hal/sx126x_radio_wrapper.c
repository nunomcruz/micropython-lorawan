// Namespace wrapper for sx126x.c + sx126x/radio.c.
// Renames the global symbols that collide with sx1276.c, and renames the
// Radio function-pointer table to Radio_SX126x so it can coexist with
// Radio_SX1276 from the sx1276 wrapper.  The originals in loramac-node/
// are NOT modified.
#define FskBandwidths   SX126x_FskBandwidths
#define RxTimeoutTimer  SX126x_RxTimeoutTimer
#define TxTimeoutTimer  SX126x_TxTimeoutTimer
#define Radio           Radio_SX126x

#include "../loramac-node/src/radio/sx126x/sx126x.c"
#include "../loramac-node/src/radio/sx126x/radio.c"
