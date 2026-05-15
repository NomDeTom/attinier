#include <Arduino.h>
#include <Wire.h>

#include "analog_util.h"
#include "battery_config.h"
#include "bq25798.h"
#include "ina3221_subordinate.h"
#include "monotonic_millis.h"

namespace {

// ATtiny412 pinmap (SOIC-8 / megaTinyCore)
//
//              ┌────────┐
//   VCC    1 ──┤        ├── 8   PA0  Arduino 0  UPDI / AIN0
//   PA6    2 ──┤        ├── 7   GND
//   PA7    3 ──┤        ├── 6   PA3  Arduino 3  SCK  / AIN3
//   PA1    4 ──┤        ├── 5   PA2  Arduino 2  MISO / RXD / AIN2
//              └────────┘
//
//  Arduino  Port  Physical  Analog  Alt functions
//        0  PA0       8      A0     UPDI (reserved for programming)
//        1  PA1       4      A1     SDA (Wire), TXD, MOSI
//        2  PA2       5      A2     SCL (Wire), RXD, MISO
//        3  PA3       6      A3     SCK, EXTCLK
//        6  PA6       2      A6     (GPIO / ADC)
//        7  PA7       3      A7     (GPIO / ADC), CLKOUT

constexpr uint8_t  kI2cSdaPin          = PIN_WIRE_SDA; // PA1, physical 4
constexpr uint8_t  kI2cSclPin          = PIN_WIRE_SCL; // PA2, physical 5
constexpr uint8_t  kChemistrySelectPin = A3;           // PA3, physical 6
constexpr uint64_t I2CPollIntervalMs   = 2000;         // Poll charger every 2 seconds
constexpr uint64_t ADCPollIntervalMs   = 250;          // Poll battery voltage every 250ms
constexpr uint8_t  kCutoffPin          = A7;           // PA7, physical 3
constexpr uint8_t  kBatteryMonitorPin  = A6;           // PA6, physical 2

Bq25798               charger;
MonotonicMillis       uptime;
BatteryChemistry      currentChemistry = BatteryChemistry::TrueDefault;
const BatteryProfile *currentProfile   = nullptr;
uint64_t              lastAdcPollMs    = 0;
uint64_t              lastI2CPollMs    = 0;

#ifdef attiny816

constexpr uint8_t  kSoftStartPin          = PIN_PB5;           // PB5, physical 9
constexpr uint8_t  kSoftStartMonitorPin  = PIN_PB4;           // PB4, physical 10
constexpr uint8_t  kSoftBusMonitorPin          = PIN_PB3;           // PB3, physical 11
constexpr uint8_t  kOutputEnablePin  = PIN_PB2;           // PB2, physical 12

#endif



// Write charge voltage and cutoff/reinstate thresholds to the charger for the current profile.
void configureBattery() {
  if (currentProfile == nullptr) {
    return;
  }
  // Charge voltage limit: (mV - 2500) / 10
  uint16_t chargeVoltageReg = (currentProfile->fullChargeVoltage - 2500u) / 10u;
  charger.writeRegister16(BQ25798_REG_CHARGE_VOLTAGE_LIMIT, chargeVoltageReg);
}

// Read battery ADC and drive the cutoff pin with hysteresis.
void measureVoltage() {
  if (currentProfile == nullptr) {
    return;
  }
  const uint16_t vcc     = readVcc();
  const uint16_t voltage = readVoltage(kBatteryMonitorPin, vcc);
  if (voltage >= currentProfile->reinstateVoltage) {
    // BatteryOK: normal operation
    digitalWrite(kCutoffPin, LOW);
  } else if (voltage < currentProfile->cutoffVoltage) {
    // BatteryUVCO: disable load
    digitalWrite(kCutoffPin, HIGH);
  }
  // BatteryLow: in hysteresis zone, do not change cutoff pin state
}

// Monitor softstart pin based on bus monitor vs softstart monitor voltage comparison.
// If softstart monitor is <90% of bus monitor, softstart pin is set to HiZ (high impedance).
// Otherwise, softstart pin is held LOW.
void monitorSoftStartPin() {
#ifdef attiny816
  const uint16_t vcc  = readVcc();
  // Positive when softstart monitor lags behind bus monitor
  const int16_t diff  = readVoltage(kSoftBusMonitorPin, kSoftStartMonitorPin, vcc);

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
  (void)kI2cSdaPin;
  (void)kI2cSclPin;

  pinMode(kCutoffPin, OUTPUT);
  digitalWrite(kCutoffPin, LOW);

#ifdef attiny816
  Wire.swap(1); // Route TWI to alternate pins: SDA=PA1 (PIN_WIRE_SDA_PINSWAP_1), SCL=PA2 (PIN_WIRE_SCL_PINSWAP_1)
#endif

  Wire.begin();
  Wire.setClock(100000);
  charger.begin(Wire);

#ifdef INA3221_EMULATOR
  charger.enableAdc(true); // Enable BQ25798 continuous ADC for INA3221 channel data
  // Enable subordinate mode on the same TWI module (dual master+subordinate).
  Wire.begin(kSubordinateI2cAddress);
  Wire.onReceive(onSubordinateReceive);
  Wire.onRequest(onSubordinateRequest);
#endif

  // Read chemistry selector, load profile, and configure charger
  currentChemistry = BatteryProfiles::selectChemistryByLevel(readVoltageLevel(kChemistrySelectPin));
  currentProfile   = BatteryProfiles::getProfile(currentChemistry);
  configureBattery();
}

void loop() {
  const uint64_t now = uptime.now();

  // ADC poll: battery cutoff check every 250ms
  if (now - lastAdcPollMs >= ADCPollIntervalMs) {
    lastAdcPollMs = now;

    // ReadChemistry
    BatteryChemistry selected =
        BatteryProfiles::selectChemistryByLevel(readVoltageLevel(kChemistrySelectPin));
    if (selected != currentChemistry) {
      currentChemistry = selected;
      currentProfile   = BatteryProfiles::getProfile(currentChemistry);
      // ConfigureBattery
      configureBattery();
    }

    // MeasureVoltage
    measureVoltage();

    // Monitor softstart pin
    monitorSoftStartPin();
  }

  // I2C poll: charger check every 2 seconds
  if (now - lastI2CPollMs >= I2CPollIntervalMs) {
    lastI2CPollMs = now;

    // CheckCharger (all operations check-then-write: read current value, only update if changed)
    charger.disableWatchdog(); // Clears watchdog bits; uses updateRegister8() internally
    // Valid settings: BQ25798_VAC_OVP_26V, BQ25798_VAC_OVP_22V,
    //                 BQ25798_VAC_OVP_12V, BQ25798_VAC_OVP_7V (default)
    charger.setVacOvp(BQ25798_VAC_OVP_26V); // Sets VAC OVP bits; uses updateRegister8() internally
    charger.enableMppt(true);               // Enables MPPT; uses updateRegister8() internally
    charger.setMinimalSystemVoltage(BQ25798_VSYSMIN_MV(3000)); // Checks then writes if different
#ifdef INA3221_EMULATOR
    updateIna3221Registers(charger);
#endif
  }
}
