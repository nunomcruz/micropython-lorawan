#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "gpio.h"
#include "spi.h"
#include "delay.h"
#include "lorawan_config.h"
#include "pin_config.h"

#include "sx126x/sx126x.h"
#include "sx126x-board.h"
#include "radio_select.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_rom_sys.h"

// Current operating mode — written by driver, read back in board callbacks
static RadioOperatingModes_t operating_mode = MODE_STDBY_RC;

// Mutex protecting all SPI bus accesses.
// Two FreeRTOS tasks concurrently access the radio SPI:
//   - lorawan_task:   RadioIrqProcess() → SX126xGetIrqStatus()
//   - esp_timer task: OnRxWindowNTimerEvent() → RxWindowSetup() → Radio.Rx()
// Without this mutex, concurrent transactions corrupt the SPI stream.
static SemaphoreHandle_t s_spi_mutex = NULL;

// Last frequency set via RADIO_SET_RFFREQUENCY. Captured for the TX diagnostic
// log in SX126xSetRfTxPower; reset on deinit so a reused module doesn't
// report a stale frequency from a previous session.
static uint32_t s_last_freq_hz = 0;

void sx126x_spi_mutex_init(void)
{
    s_spi_mutex = xSemaphoreCreateRecursiveMutex();
}

// Release the recursive SPI mutex and reset state that SX126xIoInit assumes
// at default. Called on module deinit so a subsequent lorawan.LoRaWAN(...)
// starts from a clean slate.
void sx126x_spi_mutex_deinit(void)
{
    if (s_spi_mutex) {
        vSemaphoreDelete(s_spi_mutex);
        s_spi_mutex = NULL;
    }
    operating_mode = MODE_STDBY_RC;
    s_last_freq_hz = 0;
}

// Bring up the SX126x HAL.  Wrapper exposed via radio_select.h so callers
// can bring up the chip without including sx126x-board.h (which clashes
// with sx1276.h on chip-specific register/syncword macros).
void lorawan_radio_init_sx126x(void)
{
    sx126x_spi_mutex_init();
    SX126xIoInit();
}

static inline void spi_lock(void)
{
    if (s_spi_mutex) xSemaphoreTakeRecursive(s_spi_mutex, portMAX_DELAY);
}

static inline void spi_unlock(void)
{
    if (s_spi_mutex) xSemaphoreGiveRecursive(s_spi_mutex);
}

// TCXO startup time in SX126x timer units (15.625 µs each).
// 10 ms / 15.625 µs = 640 ticks. Meshtastic-validated value for the T-Beam
// 26 MHz TCXO. 5 ms (320 ticks) was too short after Radio.Sleep() wakeup,
// causing XOSC_START_ERR and fallback to RC oscillator (±15 ppm → TX rejected
// by gateway). The SX1262 datasheet minimum is 500 µs + component startup.
#define TCXO_TIMEOUT_TICKS  640U

// WaitOnBusy hard timeout in microseconds (10 ms)
#define BUSY_TIMEOUT_US     10000LL

static inline void nss_low(void)  { GpioWrite(&SX126x.Spi.Nss, 0); }
static inline void nss_high(void) { GpioWrite(&SX126x.Spi.Nss, 1); }

void SX126xIoInit(void)
{
    GpioInit(&SX126x.Spi.Nss, (PinNames)g_lorawan_pins.nss,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1);

    GpioInit(&SX126x.Reset, (PinNames)g_lorawan_pins.reset,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1);

    GpioInit(&SX126x.BUSY, (PinNames)g_lorawan_pins.busy,
             PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);

    GpioInit(&SX126x.DIO1, (PinNames)g_lorawan_pins.dio1_1262,
             PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);

    SpiInit(&SX126x.Spi, SPI_1,
            (PinNames)g_lorawan_pins.mosi, (PinNames)g_lorawan_pins.miso,
            (PinNames)g_lorawan_pins.sclk, NC);
}

void SX126xIoIrqInit(DioIrqHandler dioIrq)
{
    if (dioIrq) {
        GpioSetInterrupt(&SX126x.DIO1, IRQ_RISING_EDGE,
                         IRQ_HIGH_PRIORITY, dioIrq);
    }
}

// Deregister DIO1 ISR only. Called from the module deinit path BEFORE
// LoRaMacDeInitialization() so that no in-flight DIO1 IRQ can dispatch into
// half-torn-down MAC state. Idempotent (GpioRemoveInterrupt is safe to call
// on an already-disabled pin), so the call inside SX126xIoDeInit below
// remains as a defensive no-op.
void SX126xIoIrqDeInit(void)
{
    GpioRemoveInterrupt(&SX126x.DIO1);
}

void SX126xIoDeInit(void)
{
    GpioRemoveInterrupt(&SX126x.DIO1);
    SpiDeInit(&SX126x.Spi);
    GpioInit(&SX126x.Spi.Nss, (PinNames)g_lorawan_pins.nss,
             PIN_INPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);
}

void SX126xIoTcxoInit(void)
{
    // T-Beam SX1262: internal TCXO powered via DIO3 at 1.8V.
    // Must be configured before any calibration or RF operation.
    // TCXO_CTRL_1_8V = 0x02; timeout in 15.625 µs units.
    SX126xSetDio3AsTcxoCtrl(TCXO_CTRL_1_8V, TCXO_TIMEOUT_TICKS);
}

void SX126xIoRfSwitchInit(void)
{
    // T-Beam SX1262: DIO2 drives the TX/RX RF switch.
    SX126xSetDio2AsRfSwitchCtrl(true);
}

void SX126xIoDbgInit(void) {}

void SX126xReset(void)
{
    GpioInit(&SX126x.Reset, (PinNames)g_lorawan_pins.reset,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 0);
    DelayMs(10);
    GpioInit(&SX126x.Reset, (PinNames)g_lorawan_pins.reset,
             PIN_OUTPUT, PIN_PUSH_PULL, PIN_NO_PULL, 1);
    DelayMs(10);
}

void SX126xWaitOnBusy(void)
{
    int64_t deadline = esp_timer_get_time() + BUSY_TIMEOUT_US;
    while (GpioRead(&SX126x.BUSY)) {
        if (esp_timer_get_time() >= deadline) {
            // Timeout — radio may be stuck; proceed anyway to avoid deadlock
            break;
        }
    }
}

void SX126xWakeup(void)
{
    // Assert NSS to wake the radio from sleep, then deassert.
    // BUSY will pulse high briefly; SX126xWaitOnBusy() blocks until ready.
    // Caller must hold spi_lock (recursive mutex allows nested takes).
    nss_low();
    SpiInOut(&SX126x.Spi, RADIO_GET_STATUS);
    nss_high();
    SX126xWaitOnBusy();
    // Radio wakes into STDBY_RC — update our tracking so we don't
    // re-trigger wakeup on every subsequent SPI command.
    SX126xSetOperatingMode(MODE_STDBY_RC);
}

// Check if the radio is sleeping and wake it up before SPI access.
// Must be called while holding spi_lock.
static inline void ensure_awake(void)
{
    if (operating_mode == MODE_SLEEP) {
        SX126xWakeup();
        SX126xAntSwOn();
    }
}

void SX126xWriteCommand(RadioCommands_t opcode, uint8_t *buffer, uint16_t size)
{
    // Capture frequency for TX diagnostic (PLL steps → Hz: freq = steps * 32e6 / 2^25)
    if (opcode == RADIO_SET_RFFREQUENCY && size == 4) {
        uint32_t steps = ((uint32_t)buffer[0] << 24) | ((uint32_t)buffer[1] << 16) |
                         ((uint32_t)buffer[2] << 8)  | buffer[3];
        s_last_freq_hz = (uint32_t)(((uint64_t)steps * 32000000ULL) >> 25);
    }

    spi_lock();
    ensure_awake();
    SX126xWaitOnBusy();
    nss_low();
    SpiInOut(&SX126x.Spi, (uint16_t)opcode);
    for (uint16_t i = 0; i < size; i++) {
        SpiInOut(&SX126x.Spi, buffer[i]);
    }
    nss_high();
    spi_unlock();
    // SX126x datasheet §14.3: BUSY goes high after NSS high; don't wait here —
    // the next WaitOnBusy call (at the start of the next command) will catch it.
}

uint8_t SX126xReadCommand(RadioCommands_t opcode, uint8_t *buffer, uint16_t size)
{
    spi_lock();
    ensure_awake();
    SX126xWaitOnBusy();
    nss_low();
    SpiInOut(&SX126x.Spi, (uint16_t)opcode);
    uint8_t status = (uint8_t)SpiInOut(&SX126x.Spi, 0x00);  // NOP → status byte
    for (uint16_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)SpiInOut(&SX126x.Spi, 0x00);
    }
    nss_high();
    spi_unlock();
    return status;
}

void SX126xWriteRegisters(uint16_t address, uint8_t *buffer, uint16_t size)
{
    spi_lock();
    ensure_awake();
    SX126xWaitOnBusy();
    nss_low();
    SpiInOut(&SX126x.Spi, RADIO_WRITE_REGISTER);
    SpiInOut(&SX126x.Spi, (address >> 8) & 0xFF);
    SpiInOut(&SX126x.Spi, address & 0xFF);
    for (uint16_t i = 0; i < size; i++) {
        SpiInOut(&SX126x.Spi, buffer[i]);
    }
    nss_high();
    spi_unlock();
}

void SX126xReadRegisters(uint16_t address, uint8_t *buffer, uint16_t size)
{
    spi_lock();
    ensure_awake();
    SX126xWaitOnBusy();
    nss_low();
    SpiInOut(&SX126x.Spi, RADIO_READ_REGISTER);
    SpiInOut(&SX126x.Spi, (address >> 8) & 0xFF);
    SpiInOut(&SX126x.Spi, address & 0xFF);
    SpiInOut(&SX126x.Spi, 0x00);  // NOP → status byte (discard)
    for (uint16_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)SpiInOut(&SX126x.Spi, 0x00);
    }
    nss_high();
    spi_unlock();
}

void SX126xWriteRegister(uint16_t address, uint8_t value)
{
    SX126xWriteRegisters(address, &value, 1);
}

uint8_t SX126xReadRegister(uint16_t address)
{
    uint8_t data;
    SX126xReadRegisters(address, &data, 1);
    return data;
}

void SX126xWriteBuffer(uint8_t offset, uint8_t *buffer, uint8_t size)
{
    spi_lock();
    ensure_awake();
    SX126xWaitOnBusy();
    nss_low();
    SpiInOut(&SX126x.Spi, RADIO_WRITE_BUFFER);
    SpiInOut(&SX126x.Spi, offset);
    for (uint8_t i = 0; i < size; i++) {
        SpiInOut(&SX126x.Spi, buffer[i]);
    }
    nss_high();
    spi_unlock();
}

void SX126xReadBuffer(uint8_t offset, uint8_t *buffer, uint8_t size)
{
    spi_lock();
    ensure_awake();
    SX126xWaitOnBusy();
    nss_low();
    SpiInOut(&SX126x.Spi, RADIO_READ_BUFFER);
    SpiInOut(&SX126x.Spi, offset);
    SpiInOut(&SX126x.Spi, 0x00);  // NOP → status byte (discard)
    for (uint8_t i = 0; i < size; i++) {
        buffer[i] = (uint8_t)SpiInOut(&SX126x.Spi, 0x00);
    }
    nss_high();
    spi_unlock();
}

void SX126xSetRfTxPower(int8_t power)
{
    // Hardware cap: SX1262 supports up to +22 dBm (enforced inside SX126xSetTxParams).
    // g_tx_power_hw_override overrides the MAC-provided value when set by the user;
    // allows exceeding the region's regulatory limit at the user's responsibility.
    int8_t p = (g_tx_power_hw_override != LORAWAN_TX_POWER_NO_OVERRIDE)
               ? g_tx_power_hw_override : power;

    // TX diagnostic: sync word, IRQ status, radio errors, and frequency.
    // Sync word 0x3444 = public (LoRaWAN); 0x1424 = private.
    // err bit 5 (0x0020) = XOSC_START_ERR: TCXO failed to start → RC oscillator used → frequency off.
    uint8_t sw0 = SX126xReadRegister(0x0740);
    uint8_t sw1 = SX126xReadRegister(0x0741);
    uint16_t irq = 0;
    SX126xReadCommand(RADIO_GET_IRQSTATUS, (uint8_t *)&irq, 2);
    uint8_t err_buf[2] = {0};
    SX126xReadCommand(RADIO_GET_ERROR, err_buf, 2);
    uint16_t errs = ((uint16_t)err_buf[0] << 8) | err_buf[1];
    esp_rom_printf("sx1262 TX: power=%d freq=%luHz syncword=0x%02x%02x irq=0x%04x err=0x%04x\n",
                   (int)p, (unsigned long)s_last_freq_hz, sw0, sw1, (unsigned)irq, (unsigned)errs);

    SX126xSetTxParams(p, RADIO_RAMP_800_US);
}

uint8_t SX126xGetDeviceId(void)
{
    // T-Beam SX1262 variant
    return SX1262;
}

void SX126xAntSwOn(void)
{
    // DIO2 controls the RF switch; SX126xSetDio2AsRfSwitchCtrl enables this
    // in hardware — no separate GPIO drive needed.
}

void SX126xAntSwOff(void)
{
    // Same as above — handled by radio hardware.
}

bool SX126xCheckRfFrequency(uint32_t frequency)
{
    // Accept full ISM range supported by SX1262 (covers EU433: 433–435 MHz, EU868: 863–870 MHz, US915, AS923, etc.)
    return (frequency >= 150000000UL && frequency <= 960000000UL);
}

uint32_t SX126xGetBoardTcxoWakeupTime(void)
{
    // 5 ms startup time for the T-Beam TCXO
    return 5;
}

uint32_t SX126xGetDio1PinState(void)
{
    return GpioRead(&SX126x.DIO1);
}

RadioOperatingModes_t SX126xGetOperatingMode(void)
{
    return operating_mode;
}

void SX126xSetOperatingMode(RadioOperatingModes_t mode)
{
    operating_mode = mode;
}
