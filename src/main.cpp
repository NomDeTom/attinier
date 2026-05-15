#include <Arduino.h>
#include <Wire.h>

#include "analog_util.h"
#include "battery_config.h"
#include "bq25798.h"
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

constexpr uint8_t kI2cSdaPin = PIN_WIRE_SDA;          // PA1, physical 4
constexpr uint8_t kI2cSclPin = PIN_WIRE_SCL;          // PA2, physical 5
constexpr uint8_t kChemistrySelectPin = A3;            // PA3, physical 6
constexpr uint64_t kPollIntervalMs = 250;
constexpr uint8_t kCutoffPin = A7;                    // PA7, physical 3
constexpr uint8_t kBatteryMonitorPin = A6;             // PA6, physical 2

Bq25798 charger;
MonotonicMillis uptime;
BatteryChemistry currentChemistry = BatteryChemistry::TrueDefault;
const BatteryProfile* currentProfile = nullptr;
uint64_t lastPollMs = 0;

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
  const uint16_t vcc = readVcc();
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

}  // namespace

void setup() {
  (void)kI2cSdaPin;
  (void)kI2cSclPin;

  pinMode(kCutoffPin, OUTPUT);
  digitalWrite(kCutoffPin, LOW);

  Wire.begin();
  Wire.setClock(100000);
  charger.begin(Wire);

  // Set minimal system voltage to 3V
  // REG00: Value = (3000mV - 2500mV) / 250mV = 2
  charger.writeRegister8(BQ25798_REG_MINIMAL_SYSTEM_VOLTAGE, 0x02);

  // Read chemistry selector, load profile, and configure charger
  currentChemistry = BatteryProfiles::selectChemistryByLevel(
      readVoltageLevel(kChemistrySelectPin));
  currentProfile = BatteryProfiles::getProfile(currentChemistry);
  configureBattery();
}

void loop() {
  const uint64_t now = uptime.now();
  if (now - lastPollMs < kPollIntervalMs) {
    return;
  }
  lastPollMs = now;

  // ReadChemistry
  BatteryChemistry selected = BatteryProfiles::selectChemistryByLevel(
      readVoltageLevel(kChemistrySelectPin));
  if (selected != currentChemistry) {
    currentChemistry = selected;
    currentProfile = BatteryProfiles::getProfile(currentChemistry);
    // ConfigureBat
    configureBattery();
  }

  // MeasureVoltage
  measureVoltage();

  // CheckCharger
  charger.disableWatchdog();
  // Valid settings: BQ25798_VAC_OVP_26V, BQ25798_VAC_OVP_22V,
  //                 BQ25798_VAC_OVP_12V, BQ25798_VAC_OVP_7V (default)
  charger.setVacOvp(BQ25798_VAC_OVP_26V);
  charger.enableMppt(true);
}
