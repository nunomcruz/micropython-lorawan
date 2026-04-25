"""
TTGO T-Beam hardware detection and configuration.
Supports: v0.7, v1.0, v1.1, v1.2 with SX1276 or SX1262, AXP192 or AXP2101.

Pin sources: Meshtastic variant.h, LILYGO official repos, confirmed schematics.
"""
from machine import Pin, SPI, I2C, UART
from time import sleep_ms

# Pins common to ALL T-Beam versions (v0.7 through v1.2)
SPI_SCLK = 5
SPI_MOSI = 27
SPI_MISO = 19
SPI_CS   = 18
LORA_RST = 23      # GPIO23 confirmed across all T-Beam versions

I2C_SDA  = 21
I2C_SCL  = 22
PMU_ADDR = 0x34    # Both AXP192 and AXP2101 share this address
PMU_IRQ  = 35
OLED_ADDR = 0x3C

BUTTON   = 38      # User button, active low
LED      = 4       # Status LED (may not exist on v0.7)

# Radio interrupt and busy pins differ by chip
_PINS_SX1276 = {"irq": 26, "busy": None}   # DIO0 = GPIO26
_PINS_SX1262 = {"irq": 33, "busy": 32}     # DIO1 = GPIO33, BUSY = GPIO32

# T-Beam SX1262 uses a TCXO on DIO3 at 1.8V.
# Without this the radio raises XOSC_START_ERR (0x20) on first configure().
# Source: Meshtastic variant.h SX126X_DIO3_TCXO_VOLTAGE = 1.8
SX1262_TCXO_MV = 1800

# GPS UART pins differ by board revision
# v1.0+: ESP32 RX=34 (input-only GPIO, ideal for RX), TX=12
# v0.7:  ESP32 RX=12, TX=15
_GPS_V10_PLUS = {"rx": 34, "tx": 12}
_GPS_V07      = {"rx": 12, "tx": 15}


class HardwareInfo:
    """Hardware detection result."""

    def __init__(self):
        self.radio = None       # "sx1276" or "sx1262"
        self.pmu = None         # "axp192", "axp2101", or None (v0.7)
        self.irq_pin = None     # radio interrupt GPIO
        self.busy_pin = None    # BUSY GPIO (SX1262 only)
        self.gps_rx = None      # GPS UART RX pin (ESP32 side)
        self.gps_tx = None      # GPS UART TX pin (ESP32 side)
        self.has_oled = False

    def __repr__(self):
        return (
            f"HardwareInfo(radio={self.radio}, pmu={self.pmu}, "
            f"irq={self.irq_pin}, busy={self.busy_pin}, "
            f"gps_rx={self.gps_rx}, gps_tx={self.gps_tx}, "
            f"oled={self.has_oled})"
        )


def _detect_radio():
    """Read SPI register 0x42 (SX1276 RegVersion). Returns 'sx1276' if 0x12, else 'sx1262'."""
    spi = SPI(1, baudrate=1_000_000, polarity=0, phase=0,
              sck=Pin(SPI_SCLK), mosi=Pin(SPI_MOSI), miso=Pin(SPI_MISO))
    cs = Pin(SPI_CS, Pin.OUT, value=1)
    rst = Pin(LORA_RST, Pin.OUT, value=1)

    rst.value(0)
    sleep_ms(10)
    rst.value(1)
    sleep_ms(10)

    cs.value(0)
    buf = bytearray(2)
    spi.write_readinto(b"\x42\x00", buf)
    cs.value(1)
    spi.deinit()
    # Release GPIOs so the C HAL can reconfigure them cleanly from scratch.
    Pin(SPI_CS, Pin.IN)
    Pin(LORA_RST, Pin.IN)

    return "sx1276" if buf[1] == 0x12 else "sx1262"


def _detect_pmu():
    """Detect PMU chip via I2C. Returns 'axp192', 'axp2101', or None for v0.7 (no PMU)."""
    i2c = I2C(0, scl=Pin(I2C_SCL), sda=Pin(I2C_SDA), freq=400_000)
    try:
        if PMU_ADDR not in i2c.scan():
            return None  # v0.7 has no I2C PMU (uses TP4054 charger + ADC divider)
        try:
            # AXP2101 chip ID register 0x03 returns 0x4A or 0x4B (>= 0x40)
            # AXP192 register 0x03 is power status, always < 0x10
            val = i2c.readfrom_mem(PMU_ADDR, 0x03, 1)[0]
            return "axp2101" if val >= 0x40 else "axp192"
        except Exception:
            return None
    finally:
        i2c.deinit()


def _detect_gps_pins(has_pmu):
    """Infer GPS UART pinout. v0.7 (no PMU) uses RX=12/TX=15; v1.0+ uses RX=34/TX=12."""
    if not has_pmu:
        return _GPS_V07

    # v1.0+ default. Try to confirm with live NMEA data.
    try:
        uart = UART(1, baudrate=9600, rx=34, tx=12, timeout=2000)
        try:
            data = uart.read(32)
        finally:
            uart.deinit()
        if data and b"$G" in data:
            return _GPS_V10_PLUS
    except Exception:
        pass

    # Default to v1.0+ pinout even if GPS is powered off or not responding.
    return _GPS_V10_PLUS


def _detect_oled():
    """Return True if an SSD1306 OLED is present on I2C address 0x3C."""
    i2c = I2C(0, scl=Pin(I2C_SCL), sda=Pin(I2C_SDA), freq=400_000)
    try:
        return OLED_ADDR in i2c.scan()
    finally:
        i2c.deinit()


def detect():
    """Detect all hardware and return a HardwareInfo instance.

    Takes ~2-4 seconds on first call because GPS UART is probed.
    Cache the result if calling multiple times.
    """
    info = HardwareInfo()
    info.radio = _detect_radio()
    info.pmu = _detect_pmu()
    info.has_oled = _detect_oled()

    if info.radio == "sx1276":
        info.irq_pin = _PINS_SX1276["irq"]
        info.busy_pin = None
    else:
        info.irq_pin = _PINS_SX1262["irq"]
        info.busy_pin = _PINS_SX1262["busy"]

    gps = _detect_gps_pins(has_pmu=(info.pmu is not None))
    info.gps_rx = gps["rx"]
    info.gps_tx = gps["tx"]

    return info


def lora_spi(baudrate=10_000_000):
    """Return the SPI bus configured for the LoRa radio."""
    return SPI(1, baudrate=baudrate, polarity=0, phase=0,
               sck=Pin(SPI_SCLK), mosi=Pin(SPI_MOSI), miso=Pin(SPI_MISO))


def lora_pins(hw_info=None):
    """Return (cs, irq, rst) for SX1276, or (cs, irq, rst, busy) for SX1262."""
    if hw_info is None:
        hw_info = detect()
    cs = Pin(SPI_CS, Pin.OUT, value=1)
    irq = Pin(hw_info.irq_pin, Pin.IN)
    rst = Pin(LORA_RST, Pin.OUT, value=1)
    if hw_info.busy_pin is not None:
        return cs, irq, rst, Pin(hw_info.busy_pin, Pin.IN)
    return cs, irq, rst


def lora_modem(hw_info=None, lora_cfg=None):
    """Construct and return the correct lora driver for detected hardware.

    Returns a SyncModem-based instance (SX1276 or SX1262) ready to configure().
    Handles SX1262 TCXO initialisation automatically.
    """
    if hw_info is None:
        hw_info = detect()
    spi = lora_spi()
    if hw_info.radio == "sx1276":
        from lora import sx127x
        cs, irq, rst = lora_pins(hw_info)
        return sx127x.SX1276(spi, cs=cs, dio0=irq, reset=rst, lora_cfg=lora_cfg)
    else:
        from lora import sx126x
        cs, irq, rst, busy = lora_pins(hw_info)
        return sx126x.SX1262(spi, cs=cs, dio1=irq, reset=rst, busy=busy,
                              dio3_tcxo_millivolts=SX1262_TCXO_MV,
                              lora_cfg=lora_cfg)


def gps_uart(hw_info=None, baudrate=9600):
    """Return a UART configured for the GPS module."""
    if hw_info is None:
        hw_info = detect()
    return UART(1, baudrate=baudrate, rx=hw_info.gps_rx, tx=hw_info.gps_tx)


def i2c_bus(freq=400_000):
    """Return the I2C bus (PMU, OLED, sensors)."""
    return I2C(0, scl=Pin(I2C_SCL), sda=Pin(I2C_SDA), freq=freq)
