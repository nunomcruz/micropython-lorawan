Single firmware for all TTGO T-Beam variants with automatic hardware detection at boot.

Supported variants:

- **v0.7** — SX1276, no PMU (TP4054 charger), GPS on RX=12/TX=15
- **v1.0** — SX1276, AXP192 PMU, GPS on RX=34/TX=12
- **v1.1** — SX1276 or SX1262, AXP192 PMU, GPS on RX=34/TX=12
- **v1.2** — SX1276 or SX1262, AXP2101 PMU, GPS on RX=34/TX=12

Import `tbeam` to detect hardware at runtime: radio chip (SX1276/SX1262), PMU model,
GPS UART pins, and OLED presence.
