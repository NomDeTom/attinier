#include <Arduino.h>
#include <Wire.h>
#include <avr/sleep.h>

#ifdef BQ25798_USE_SOFT_I2C
#include <SlowSoftI2CMaster.h>
#endif

#include "analog_util.h"
#include "battery_config.h"
#include "bq25798.h"
#include "ina3221_subordinate.h"

namespace {

// ATtiny412 pinmap (SOIC-8 / megaTinyCore)
//
//                          ┌────────┐
//               VCC    1 ──┤        ├── 8   GND
// DAC/AIN6  0~  PA6    2 ──┤        ├── 7   PA3  4~  SCK/CLKI/AIN3
//     AIN7  1~  PA7    3 ──┤        ├── 6   PA0   5  UPDI/AIN0
// SDA/AIN1  2~  PA1    4 ──┤        ├── 5   PA2  3~  SCL/AIN2
//                          └────────┘
//
//  Arduino  Port  Physical  Analog  Alt functions
//       0~  PA6       2      A6     DAC
//       1~  PA7       3      A7     (GPIO / ADC)
//       2~  PA1       4      A1     SDA (Wire)
//       3~  PA2       5      A2     SCL (Wire), async INT
//       4~  PA3       7      A3     SCK, CLKI (ext clock)
//        5  PA0       6      A0     UPDI (reserved for programming)

#if defined(__AVR_ATtiny412__) && defined(BQ25798_USE_SOFT_I2C)
constexpr uint8_t  kSubordinateI2cSdaPin = PIN_WIRE_SDA; // PA1, physical 4
constexpr uint8_t  kSubordinateI2cSclPin = PIN_WIRE_SCL; // PA2, physical 5
constexpr uint8_t  kChargerI2cSdaPin     = A6;           // PA6, physical 2
constexpr uint8_t  kChargerI2cSclPin     = A7;           // PA7, physical 3
constexpr uint8_t  kCutoffPin            = A3;           // PA3, physical 7
#elif defined(BQ25798_USE_SOFT_I2C) && defined(PIN_WIRE_SDA_PINSWAP_1) && defined(PIN_WIRE_SCL_PINSWAP_1)
constexpr uint8_t  kSubordinateI2cSdaPin = PIN_WIRE_SDA_PINSWAP_1;
constexpr uint8_t  kSubordinateI2cSclPin = PIN_WIRE_SCL_PINSWAP_1;
constexpr uint8_t  kChargerI2cSdaPin     = PIN_WIRE_SDA;
constexpr uint8_t  kChargerI2cSclPin     = PIN_WIRE_SCL;
constexpr uint8_t  kCutoffPin            = A7; // PA7, physical 3
#else
constexpr uint8_t  kSubordinateI2cSdaPin = PIN_WIRE_SDA;
constexpr uint8_t  kSubordinateI2cSclPin = PIN_WIRE_SCL;
constexpr uint8_t  kCutoffPin            = A7; // PA7, physical 3
#endif
constexpr uint32_t I2CPollIntervalMs   = 2000;         // Poll charger every 2 seconds
constexpr uint32_t ADCPollIntervalMs   = 250;          // Poll battery voltage every 250ms

Bq25798               charger;
#ifdef BQ25798_USE_SOFT_I2C
SlowSoftI2CMaster     chargerBus(kChargerI2cSdaPin, kChargerI2cSclPin, false);
#endif
BatteryChemistry      currentChemistry = BatteryChemistry::TrueDefault;
const BatteryProfile *currentProfile   = nullptr;
bool                  chargerPresent   = false;
bool                  chargerEverSeen  = false;
bool                  cutoffActive     = false; // mirrors kCutoffPin; HIGH = load disabled

// RTC PIT tick-based timing (PIT period = 8192 RTC cycles = exactly 250 ms at 32.768 kHz).
volatile bool adcPollDue       = false; // set by PIT ISR every 250 ms
uint8_t       i2cPollTick      = 0;     // 0–7; I²C poll fires when it reaches 8 (2000 ms)
uint8_t       chargerLostPolls = 0;     // I²C polls since charger lost; ≥5 → 10 s elapsed

// Initialise the RTC Periodic Interrupt Timer to fire every 250 ms (8192 cycles at 32.768 kHz).
// The PIT uses OSCULP32K which stays active in STANDBY sleep, so it wakes the CPU without
// requiring the main oscillator to run.
inline void initRtcPit() {
  RTC.CLKSEL     = RTC_CLKSEL_INT32K_gc; // 32.768 kHz internal oscillator
  RTC.PITINTCTRL = RTC_PI_bm;            // enable PIT interrupt
  while (RTC.PITSTATUS & RTC_CTRLBUSY_bm)
    ;                                  // wait for PIT register sync
  RTC.PITCTRLA = RTC_PERIOD_CYC8192_gc // 8192 cycles = 250 ms
                 | RTC_PITEN_bm;       // enable PIT
}

ISR(RTC_PIT_vect) {
  adcPollDue      = true;
  RTC.PITINTFLAGS = RTC_PI_bm; // clear interrupt flag
}

// ATtiny816 pinmap (VQFN-20 / megaTinyCore)
//
//            +----------T20 PA1
//            | +--------T19 PA0
//            | | +------T18 PC3
//            | | |  +---T17 PC2
//            | | |  | +-T16 PC1
//            | | |  | |
//          +-+-+-+--+-+-+
//  1 PA2 --|            |-- PC0 15
//  2 PA3 --|  ATtiny816 |-- PB0 14
//  3 GND --|   VQFN-20  |-- PB1 13
//  4 VDD --|            |-- PB2 12
//  5 PA4 --|            |-- PB3 11
//          +-+-+-+--+-+-+
//            | | |  | |
//            | | |  | +-B10 PB4
//            | | |  +---B 9 PB5
//            | | +------B 8 PA7
//            | +--------B 7 PA6
//            +----------B 6 PA5
//
//  Arduino  Port  Physical  Analog  Alt functions
//      15  PA2       1      A2     MISO; SCL_alt  (active TWI SCL via Wire.swap(1))
//      16~  PA3       2      A3     SCK, CLKI (ext clock)
//       --  GND       3      --
//       --  VDD       4      --
//       0~  PA4       5      A4
//       1~  PA5       6      A5     VREFA
//       2~  PA6       7      A6     DAC
//       3~  PA7       8      A7
//        4  PB5       9      A8     CLKOUT
//        5  PB4      10      A9
//        6  PB3      11      --     UART0 RX (default)
//       7~  PB2      12      --     UART0 TX (default)
//       8~  PB1      13     A10     SDA (Wire default; inactive — Wire.swap(1) in use)
//       9~  PB0      14     A11     SCL (Wire default; inactive — Wire.swap(1) in use)
//      10~  PC0      15      --     WOC (TCD0 PWM when enabled)
//      11~  PC1      16      --     WOD (TCD0 PWM when enabled)
//       12  PC2      17      --
//       13  PC3      18      --
//       17  PA0      19      A0     UPDI (reserved for programming)
//       14  PA1      20      A1     MOSI; SDA_alt  (active TWI SDA via Wire.swap(1))

#ifdef attiny816

constexpr uint8_t kSoftStartPin        = PIN_PB5; // PB5, Arduino 4, physical 9
constexpr uint8_t kSoftStartMonitorPin = PIN_PB4; // PB4, Arduino 5, physical 10
constexpr uint8_t kSoftBusMonitorPin   = PIN_PB3; // PB3, Arduino 6, physical 11
constexpr uint8_t kOutputEnablePin     = PIN_PB2; // PB2, Arduino 7, physical 12
constexpr uint8_t kAuxBusPin =
    PIN_PA6; // PA6, Arduino 2, physical 7 — aux bus voltage (VCC must be ≥ max bus V)
constexpr uint8_t kAuxShuntPin =
    PIN_PA7; // PA7, Arduino 3, physical 8 — aux shunt low side (1 Ω shunt)
constexpr uint8_t kAuxOversampleLog2 =
    6u; // 64 interleaved pairs — ~3 ms, ~0.9 mA resolution at 5 V/1 Ω

#endif

// Write charge voltage and cutoff/reinstate thresholds to the charger for the current profile.
void configureBattery() {
  if (currentProfile == nullptr) {
    return;
  }
  charger.writeRegister16(BQ25798_REG_CHARGE_VOLTAGE_LIMIT, currentProfile->chargeReg);
}

bool readChargerAdc(uint8_t reg, uint16_t &out) {
  uint8_t hi = 0u;
  uint8_t lo = 0u;
  if (!charger.readRegister8(reg, hi) || !charger.readRegister8(reg + 1u, lo)) {
    return false;
  }
  out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

uint8_t readChemistryLevel() {
#if defined(__AVR_ATtiny412__) && defined(BQ25798_USE_SOFT_I2C)
  uint16_t vac1 = 0u;
  uint16_t vac2 = 0u;
  if (!chargerPresent || !readChargerAdc(BQ25798_REG_VAC1_ADC, vac1) ||
      !readChargerAdc(BQ25798_REG_VAC2_ADC, vac2) || vac1 == 0u) {
    return static_cast<uint8_t>(BatteryChemistry::TrueDefault);
  }
  const uint32_t scaled = static_cast<uint32_t>(vac2) * 7u + (vac1 / 2u);
  const uint8_t  level  = static_cast<uint8_t>(scaled / vac1);
  return (level > 7u) ? 7u : level;
#else
  return readVoltageLevel(A3);
#endif
}

bool beginCharger() {
#ifdef BQ25798_USE_SOFT_I2C
  return chargerBus.i2c_init() && charger.begin(chargerBus);
#else
  return charger.begin(Wire);
#endif
}

// Read battery ADC and drive the cutoff pin with hysteresis.
void measureVoltage() {
  if (currentProfile == nullptr) {
    return;
  }
  uint16_t voltage = 0u;
#if defined(__AVR_ATtiny412__) && defined(BQ25798_USE_SOFT_I2C)
  if (!chargerPresent || !readChargerAdc(BQ25798_REG_VBAT_ADC, voltage)) {
    return;
  }
#else
  const uint16_t vcc = readVcc();
  voltage            = readVoltage(A6, vcc);
#endif
  if (voltage >= currentProfile->reinstateMv()) {
    // BatteryOK: normal operation
    if (cutoffActive) {
      cutoffActive = false;
#ifdef attiny816
      pinMode(kSoftStartPin, INPUT); // HiZ on cutoff release — restart softstart monitoring
#endif
    }
    digitalWrite(kCutoffPin, LOW);
  } else if (voltage < currentProfile->cutoffMv()) {
    // BatteryUVCO: disable load
    if (!cutoffActive) {
      cutoffActive = true;
#ifdef attiny816
      pinMode(kSoftStartPin, INPUT); // HiZ on cutoff activation
#endif
    }
    digitalWrite(kCutoffPin, HIGH);
  }
  // BatteryLow: in hysteresis zone, do not change cutoff pin state
}

#if defined(attiny816) && defined(INA3221_EMULATOR)
// Measure auxiliary input shunt (PA6 high side, PA7 low side) and update
// INA3221 channel 2 shadow registers.
// Bus voltage: 8-sample average (INA3221 bus LSB = 8 mV; no benefit in more samples).
// Shunt voltage: 64-pair interleaved oversample — ~0.9 mA noise floor at 5 V / 1 Ω.
void measureAuxChannel() {
  const uint16_t vcc    = readVcc();
  const uint16_t busMv  = readVoltageAvg(kAuxBusPin, vcc, 3u);
  const int16_t  diffMv = readDifferentialMv(kAuxBusPin, kAuxShuntPin, vcc, kAuxOversampleLog2);
  updateIna3221Ch2(busMv, (int32_t)diffMv * 1000);
}
#endif

// Monitor softstart pin based on bus monitor vs softstart monitor voltage comparison.
// If softstart monitor is <90% of bus monitor, softstart pin is set to HiZ (high impedance).
// Otherwise, softstart pin is held LOW.
void monitorSoftStartPin() {
#ifdef attiny816
  const uint16_t vcc = readVcc();
  // Positive when softstart monitor lags behind bus monitor
  const int16_t diff = readVoltage(kSoftBusMonitorPin, kSoftStartMonitorPin, vcc);

  if (diff > 10) {
    // Softstart monitor is below 90% of bus monitor: set to HiZ (high impedance)
    pinMode(kSoftStartPin, INPUT);
  } else {
    // Softstart monitor is at or above 90% of bus monitor: hold LOW
    pinMode(kSoftStartPin, OUTPUT);
    digitalWrite(kSoftStartPin, LOW);
  }
#endif
}

} // namespace

void setup() {
  (void)kSubordinateI2cSdaPin;
  (void)kSubordinateI2cSclPin;

  pinMode(kCutoffPin, OUTPUT);
  digitalWrite(kCutoffPin, LOW);

#ifdef attiny816
  pinMode(kSoftStartPin, INPUT); // HiZ until first voltage differential confirms softstart complete
#endif

#if defined(BQ25798_USE_SOFT_I2C) && defined(PIN_WIRE_SDA_PINSWAP_1) && defined(PIN_WIRE_SCL_PINSWAP_1)
  Wire.swap(1); // Route TWI to alternate pins: SDA=PA1 (PIN_WIRE_SDA_PINSWAP_1), SCL=PA2
                // (PIN_WIRE_SCL_PINSWAP_1)
#endif

#ifdef INA3221_EMULATOR
  // Hardware TWI serves only the INA3221 subordinate bus.
  Wire.begin(kSubordinateI2cAddress);
  Wire.onReceive(onSubordinateReceive);
  Wire.onRequest(onSubordinateRequest);
#ifndef BQ25798_USE_SOFT_I2C
  // Shared-bus fallback: add master mode only where the charger still rides on Wire.
  Wire.begin(); // add master (MANDS dual mode)
#endif
#else
  Wire.begin();
#endif

#ifndef BQ25798_USE_SOFT_I2C
  Wire.setClock(100000);
#endif
  chargerPresent = beginCharger();
  if (chargerPresent)
    chargerEverSeen = true;

  if (chargerPresent) {
#ifdef INA3221_EMULATOR
    charger.enableAdc(true);
#endif
  }

  currentChemistry = BatteryProfiles::selectChemistryByLevel(readChemistryLevel());
  currentProfile   = BatteryProfiles::getProfile(currentChemistry);

  if (chargerPresent) {
    configureBattery();
  }

  // Pull-ups after Wire.begin() so Wire pin reconfiguration does not clear them.
  pinMode(kSubordinateI2cSdaPin, INPUT_PULLUP);
  pinMode(kSubordinateI2cSclPin, INPUT_PULLUP);

  // STANDBY sleep: CPU halted, OSCULP32K and TWI address-match remain active.
  // RTC PIT wakes every 250 ms for ADC work; TWI interrupt wakes for I²C slave responses.
  set_sleep_mode(SLEEP_MODE_STANDBY);
  initRtcPit();
}

void loop() {
  // ADC poll: fires every PIT tick (250 ms)
  if (adcPollDue) {
    adcPollDue = false;

    // ReadChemistry
    BatteryChemistry selected = BatteryProfiles::selectChemistryByLevel(readChemistryLevel());
    if (selected != currentChemistry) {
      currentChemistry = selected;
      currentProfile   = BatteryProfiles::getProfile(currentChemistry);
      if (chargerPresent) {
        configureBattery();
      }
    }

    // MeasureVoltage
    measureVoltage();

    // Monitor softstart pin
    monitorSoftStartPin();

#if defined(attiny816) && defined(INA3221_EMULATOR)
    measureAuxChannel();
#endif

    // I2C poll: charger check every 8 ticks (2000 ms)
    if (++i2cPollTick < 8)
      goto sleep;
    i2cPollTick = 0;

    if (!chargerPresent) {
      // Re-probe using begin(): reads part ID to confirm identity, not just ACK.
      chargerPresent = beginCharger();
      if (chargerPresent) {
        chargerEverSeen  = true;
        chargerLostPolls = 0;
#ifdef INA3221_EMULATOR
        charger.enableAdc(true);
#endif
        currentChemistry = BatteryProfiles::selectChemistryByLevel(readChemistryLevel());
        currentProfile   = BatteryProfiles::getProfile(currentChemistry);
        configureBattery();
      }
    }

    if (chargerPresent) {
      // CheckCharger: chain all calls — any failure marks charger lost for re-probe next poll.
      const bool ok = charger.disableWatchdog() &&
                      // Valid settings: BQ25798_VAC_OVP_26V, BQ25798_VAC_OVP_22V,
                      //                 BQ25798_VAC_OVP_12V, BQ25798_VAC_OVP_7V (default)
                      charger.setVacOvp(BQ25798_VAC_OVP_26V) && charger.enableMppt(true) &&
                      charger.setMinimalSystemVoltage(BQ25798_VSYSMIN_MV(3000));
      if (!ok) {
        chargerPresent   = false;
        chargerLostPolls = 0; // reset counter so timeout counts from now
      }
    }

#ifdef INA3221_EMULATOR
    if (chargerPresent) {
      updateIna3221Registers(charger);
    } else if (!chargerEverSeen) {
      updateIna3221DummyValues(1000u, 10); // 1 V / 10 mA  — charger never seen
    } else if (++chargerLostPolls >= 5) {
      updateIna3221DummyValues(2000u, 20); // 2 V / 20 mA  — lost > 10 s (5 × 2000 ms)
      chargerLostPolls = 5;                // saturate so it keeps reporting
    }
    // else: charger lost < 10 s — hold last real values, no update
#endif
  }

sleep:
  // Sleep until next PIT tick (250 ms) or TWI address-match interrupt.
  sleep_mode();
}
