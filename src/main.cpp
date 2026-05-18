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

// ATtiny816 pinmap (VQFN-20 / megaTinyCore)
//
//            ┌───────── 20 PA1
//            │ ┌─────── 19 PA0
//            │ │ ┌───── 18 PC3
//            │ │ │ ┌─── 17 PC2
//            │ │ │ │ ┌─ 16 PC1
//            │ │ │ │ │
//          ┌─┴─┴─┴─┴─┴─┐
// PA2  1 ──┤           ├── 15   PC0
// PA3  2 ──┤           ├── 14   PB0
// GND  3 ──┤ ATtiny816 ├── 13   PB1
// VDD  4 ──┤           ├── 12   PB2
// PA4  5 ──┤           ├── 11   PB3
//          └─┬─┬─┬─┬─┬─┘
//            │ │ │ │ │
//            │ │ │ │ └─ 10 PB4
//            │ │ │ └───  9 PB5
//            │ │ └─────  8 PA7
//            │ └───────  7 PA6
//            └─────────  6 PA5
//
//  Arduino  Port  Physical  Analog  Function
//       14  PA1      20      A1     SDA (INA3221 subordinate, Wire.swap(1))
//       15  PA2       1      A2     SCL (INA3221 subordinate, Wire.swap(1))
//      16~  PA3       2      A3     Chemistry select (direct ADC divider)
//       --  GND       3      --     GND
//       --  VDD       4      --     VDD
//       0~  PA4       5      A4     Main battery voltage (3:1 divider)
//       1~  PA5       6      A5     Backup battery voltage (3:1 divider)
//       2~  PA6       7      A6     TS emulation (DAC or Hi-Z)
//       3~  PA7       8      A7     Backup battery current (INA180 output)
//        4  PB5       9      A8     Supercap voltage (3:1 divider)
//        5  PB4      10      A9     Supercap bus voltage (3:1 divider)
//        6  PB3      11      --     Supercap P-FET gate (LOW = enable, Hi-Z = disable)
//       7~  PB2      12      --     (unused)
//       8~  PB1      13     A10     SDA (BQ25798 master, SlowSoftI2CMaster)
//       9~  PB0      14     A11     SCL (BQ25798 master, SlowSoftI2CMaster)
//      10~  PC0      15      --     Test switch input (LOW = switch pressed)
//      11~  PC1      16      --     Test indicator output (HIGH when switch pressed)
//       12  PC2      17      --     Backup battery enable (LOW = enable, HIGH = disable)
//       13  PC3      18      --     Main battery output enable (LOW = enable, HIGH = disable)
//       17  PA0      19      A0     UPDI (reserved for programming)
//       --   --      --      --     --

namespace {
constexpr uint8_t kSubordinateI2cSdaPin  = PIN_WIRE_SDA_PINSWAP_1;
constexpr uint8_t kSubordinateI2cSclPin  = PIN_WIRE_SCL_PINSWAP_1;
constexpr uint8_t kChargerI2cSdaPin      = PIN_WIRE_SDA; // PB1, SDA for BQ25798 (SlowSoftI2CMaster)
constexpr uint8_t kChargerI2cSclPin      = PIN_WIRE_SCL; // PB0, SCL for BQ25798 (SlowSoftI2CMaster)
constexpr uint8_t kChemistrySelectPin    = A3;           // PA3, direct chemistry selector ladder
constexpr uint8_t kMainBatterySensePin   = A4; // PA4, 3:1 divider for main battery voltage
constexpr uint8_t kBackupBatterySensePin = A5; // PA5, 3:1 divider for backup battery voltage
constexpr uint8_t kTsEmulationPin        = A6; // PA6, DAC output / Hi-Z to BQ25798 TS pin
constexpr uint8_t kBackupCurrentPin      = A7; // PA7, INA180 current monitor output
constexpr uint8_t kSupercapMosfetPin =
    PIN_PB3; // PB3, P-FET gate: LOW = enable supercap path, Hi-Z = disable
constexpr uint8_t kSupercapBusSensePin = PIN_PB4; // PB4, 3:1 divider for bus voltage
constexpr uint8_t kSupercapCapSensePin = PIN_PB5; // PB5, 3:1 divider for supercap voltage
constexpr uint8_t kBackupBatteryEnablePin =
    PIN_PC2; // PC2, backup battery enable: LOW = enable, HIGH = disable
constexpr uint8_t kMainBatteryOutputEnablePin =
    PIN_PC3; // PC3, main battery output enable: LOW = enable, HIGH = disable
constexpr uint8_t  kTestSwitchPin    = PIN_PC0; // PC0, test switch input (with pullup)
constexpr uint8_t  kTestIndicatorPin = PIN_PC1; // PC1, test indicator output
constexpr uint32_t I2CPollIntervalMs = 2000;    // Poll charger every 2 seconds
constexpr uint32_t ADCPollIntervalMs = 250;     // Poll battery voltage every 250ms
constexpr uint8_t  kI2CPollTicks     = I2CPollIntervalMs / ADCPollIntervalMs;
constexpr uint32_t kChargerLostTimeoutMs =
    10000; // After 10 seconds without the charger, report fallback INA3221 values
constexpr uint8_t  kChargerLostPollLimit      = kChargerLostTimeoutMs / I2CPollIntervalMs;
constexpr uint16_t kChargerNeverSeenBusMv     = 1000u;
constexpr int16_t  kChargerNeverSeenMa        = 10;
constexpr uint16_t kChargerLostBusMv          = 2000u;
constexpr int16_t  kChargerLostMa             = 20;
constexpr uint8_t  kPrechargeControlMask      = 0xFFu;
constexpr uint8_t  kLowVoltageChargeTimerMask = static_cast<uint8_t>((1u << 5) | (1u << 4));
constexpr uint8_t  kDefaultPrechargeControl   = static_cast<uint8_t>(
    (static_cast<uint8_t>(BQ25798_VBAT_LOWV_71_4_PERCENT) << 6) | BQ25798_IPRECHG_120MA);
constexpr uint8_t kBypassLowVoltagePrechargeControl = static_cast<uint8_t>(
    (static_cast<uint8_t>(BQ25798_VBAT_LOWV_15_PERCENT) << 6) | BQ25798_IPRECHG_2000MA);
constexpr uint8_t kSupercapEnablePct = 95u;
constexpr uint8_t kTsWarmDacCode =
    192u; // <44% of 5V REGN = <88% of 2.5V DAC reference, set at 75% for safety

static_assert(ADCPollIntervalMs != 0, "ADC poll interval must be non-zero");
static_assert(I2CPollIntervalMs % ADCPollIntervalMs == 0,
              "I2C poll interval must be an integer multiple of the ADC poll interval");
static_assert(kI2CPollTicks > 0, "I2C poll tick count must be non-zero");
static_assert(I2CPollIntervalMs != 0, "I2C poll interval must be non-zero");
static_assert(kChargerLostTimeoutMs % I2CPollIntervalMs == 0,
              "Charger-lost timeout must be an integer multiple of the I2C poll interval");
static_assert(kChargerLostPollLimit > 0, "Charger-lost poll limit must be non-zero");
static_assert((kDefaultPrechargeControl & kPrechargeControlMask) == kDefaultPrechargeControl,
              "Default precharge control must fit the precharge control register");
static_assert((kBypassLowVoltagePrechargeControl & kPrechargeControlMask) ==
                  kBypassLowVoltagePrechargeControl,
              "Bypass precharge control must fit the precharge control register");

struct LowVoltageChargeRegisterConfig {
  uint8_t prechargeControl;
  uint8_t timerControl;
};

Bq25798 charger;
#ifdef BQ25798_USE_SOFT_I2C
SlowSoftI2CMaster chargerBus(kChargerI2cSdaPin, kChargerI2cSclPin, false);
#endif
BatteryChemistry      currentChemistry = BatteryChemistry::TrueDefault;
const BatteryProfile *currentProfile   = nullptr;
bool                  chargerPresent   = false;
bool                  chargerEverSeen  = false;
bool                  mainBatteryOutputEnabled =
    false; // mirrors kMainBatteryOutputEnablePin; LOW = enabled, HIGH = disabled

// RTC PIT tick-based timing (PIT period = 8192 RTC cycles = exactly 250 ms at 32.768 kHz).
volatile bool adcPollDue  = false; // set by PIT ISR every 250 ms
uint8_t       i2cPollTick = 0; // 0..kI2CPollTicks-1; I2C poll fires every kI2CPollTicks ADC ticks
uint8_t       chargerLostPolls =
    0; // I2C polls since charger lost; >=kChargerLostPollLimit means the timeout elapsed

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

// Write charge voltage and cutoff/reinstate thresholds to the charger for the current profile.
LowVoltageChargeRegisterConfig getLowVoltageChargeRegisterConfig(const BatteryProfile &profile) {
  switch (profile.lowVoltageChargePolicy) {
  case LowVPreCharge::Full:
    return {kBypassLowVoltagePrechargeControl, 0u};
  case LowVPreCharge::Reduced:
  default:
    return {kDefaultPrechargeControl, kLowVoltageChargeTimerMask};
  }
}

void configureBattery() {
  if (currentProfile == nullptr) {
    return;
  }

  // Low-voltage entry behavior now comes from battery_config.h so each profile owns whether it
  // keeps the battery-safe reduced-current path or bypasses it for supercapacitor/LTO use cases.
  const LowVoltageChargeRegisterConfig lowVoltageChargeConfig =
      getLowVoltageChargeRegisterConfig(*currentProfile);

  charger.writeRegister16(BQ25798_REG_CHARGE_VOLTAGE_LIMIT, currentProfile->chargeReg);
  charger.updateRegister8(BQ25798_REG_PRECHARGE_CONTROL, kPrechargeControlMask,
                          lowVoltageChargeConfig.prechargeControl);
  charger.updateRegister8(BQ25798_REG_TIMER_CONTROL, kLowVoltageChargeTimerMask,
                          lowVoltageChargeConfig.timerControl);
}

void disableTsEmulation() {
  DAC0.CTRLA = 0;
  pinMode(kTsEmulationPin, INPUT);
}

void enableTsWarmEmulation() {
  VREF.CTRLA = (VREF.CTRLA & ~VREF_DAC0REFSEL_gm) | VREF_DAC0REFSEL_2V5_gc;
  DAC0.DATA  = kTsWarmDacCode;
  DAC0.CTRLA = DAC_ENABLE_bm | DAC_OUTEN_bm;
}

bool configureThermalProfile() {
  if (!chargerPresent) {
    disableTsEmulation();
    return false;
  }

  if (currentChemistry == BatteryChemistry::LTO) {
    enableTsWarmEmulation();
    return charger.updateRegister8(BQ25798_REG_NTC_CONTROL_0, 0xFEu,
                                   static_cast<uint8_t>((BQ25798_JEITA_VSET_M100MV << 5) |
                                                        (BQ25798_JEITA_ISETH_UNCHANGED << 3) |
                                                        (BQ25798_JEITA_ISETC_UNCHANGED << 1))) &&
           charger.updateRegister8(BQ25798_REG_NTC_CONTROL_1, 0x31u,
                                   static_cast<uint8_t>(BQ25798_TS_WARM_48_4PCT << 4));
  }

  disableTsEmulation();
  return charger.updateRegister8(BQ25798_REG_NTC_CONTROL_0, 0xFEu,
                                 static_cast<uint8_t>((BQ25798_JEITA_VSET_UNCHANGED << 5) |
                                                      (BQ25798_JEITA_ISETH_UNCHANGED << 3) |
                                                      (BQ25798_JEITA_ISETC_UNCHANGED << 1))) &&
         charger.updateRegister8(BQ25798_REG_NTC_CONTROL_1, 0x31u,
                                 static_cast<uint8_t>(BQ25798_TS_WARM_44_8PCT << 4));
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
  return readVoltageLevel(kChemistrySelectPin);
}

bool beginCharger() {
#ifdef BQ25798_USE_SOFT_I2C
  return chargerBus.i2c_init() && charger.begin(chargerBus);
#else
  return charger.begin(Wire);
#endif
}

uint16_t readMainBatteryVoltage() {
  return static_cast<uint16_t>(readDividerVoltage(kMainBatterySensePin));
}

int16_t readMainBatteryCurrent() {
  uint16_t ibatRaw = 0u;
  if (!chargerPresent || !readChargerAdc(BQ25798_REG_IBAT_ADC, ibatRaw)) {
    return 0;
  }
  return static_cast<int16_t>(ibatRaw);
}

uint16_t readBackupBatteryVoltage() {
  return static_cast<uint16_t>(readDividerVoltage(kBackupBatterySensePin));
}

int16_t readBackupBatteryCurrent() {
  return readINA180A2Current(kBackupCurrentPin);
}

uint16_t readSupercapBusVoltage() {
  return static_cast<uint16_t>(readDividerVoltage(kSupercapBusSensePin));
}

uint16_t readSupercapVoltage() {
  return static_cast<uint16_t>(readDividerVoltage(kSupercapCapSensePin));
}

// Drive the main and backup battery enable pins as complementary outputs.
void setBatteryOutputEnablePins(bool enableMainBattery) {
  mainBatteryOutputEnabled = enableMainBattery;
  digitalWrite(kMainBatteryOutputEnablePin, enableMainBattery ? LOW : HIGH);
  digitalWrite(kBackupBatteryEnablePin, enableMainBattery ? HIGH : LOW);
}

// Read battery ADC and drive the battery output enable pins with hysteresis.
void measureVoltage() {
  if (currentProfile == nullptr) {
    return;
  }
  const uint16_t voltage = readMainBatteryVoltage();
  if (voltage >= currentProfile->reinstateMv()) {
    // BatteryOK: normal operation
    if (!mainBatteryOutputEnabled) {
      setBatteryOutputEnablePins(true);
    }
  } else if (voltage < currentProfile->cutoffMv()) {
    // BatteryUVCO: disable load
    if (mainBatteryOutputEnabled) {
      setBatteryOutputEnablePins(false);
    }
  }
  // BatteryLow: in hysteresis zone, do not change battery output enable state
}

void updateDirectIna3221Channels() {
#ifdef INA3221_EMULATOR
  updateIna3221Ch1(readMainBatteryVoltage(), readMainBatteryCurrent());
  updateIna3221Ch2(readBackupBatteryVoltage(),
                   (int32_t)readBackupBatteryCurrent() * kShuntMilliohms);
#endif
}

// PB3 drives a P-FET gate. Driving LOW enables the supercap path; INPUT leaves the gate
// high through external pull-up hardware and disables the path without forcing the node.
void enableSupercapPath() {
  digitalWrite(kSupercapMosfetPin, LOW);
  pinMode(kSupercapMosfetPin, OUTPUT);
}

void disableSupercapPath() {
  digitalWrite(kSupercapMosfetPin, LOW); // ensure the internal pull-up stays disabled in INPUT mode
  pinMode(kSupercapMosfetPin, INPUT);
}

void monitorSupercapCharging() {
  const uint16_t busMv = readSupercapBusVoltage();
  const uint16_t capMv = readSupercapVoltage();
  if (busMv == 0u) {
    disableSupercapPath();
    return;
  }

  const uint32_t capPct = static_cast<uint32_t>(capMv) * 100u;
  const uint32_t busPct = static_cast<uint32_t>(busMv);

  if (capPct >= static_cast<uint32_t>(kSupercapEnablePct) * busPct) {
    enableSupercapPath();
  } else {
    disableSupercapPath();
  }
}

} // namespace

void setup() {
  (void)kSubordinateI2cSdaPin;
  (void)kSubordinateI2cSclPin;

  pinMode(kMainBatteryOutputEnablePin, OUTPUT);
  pinMode(kBackupBatteryEnablePin, OUTPUT);
  pinMode(kTestSwitchPin, INPUT_PULLUP);
  pinMode(kTestIndicatorPin, OUTPUT);
  digitalWrite(kTestIndicatorPin, LOW);
  setBatteryOutputEnablePins(true);
  disableSupercapPath();
  disableTsEmulation();

  Wire.swap(1); // Route TWI to alternate pins: SDA=PA1 (PIN_WIRE_SDA_PINSWAP_1), SCL=PA2
                // (PIN_WIRE_SCL_PINSWAP_1)

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
    (void)configureThermalProfile();
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

    updateDirectIna3221Channels();

    // Supercap connect control
    monitorSupercapCharging();

    // Test switch response: if test switch is pressed (PC0 pulled low), set indicator high
    if (digitalRead(kTestSwitchPin) == LOW) {
      digitalWrite(kTestIndicatorPin, HIGH);
    } else {
      digitalWrite(kTestIndicatorPin, LOW);
    }

    // I2C poll: charger check every kI2CPollTicks ADC ticks.
    if (++i2cPollTick < kI2CPollTicks)
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
        (void)configureThermalProfile();
      }
    }

    if (chargerPresent) {
      // CheckCharger: chain all calls — any failure marks charger lost for re-probe next poll.
      const bool ok = charger.disableWatchdog() &&
                      // Valid settings: BQ25798_VAC_OVP_26V, BQ25798_VAC_OVP_22V,
                      //                 BQ25798_VAC_OVP_12V, BQ25798_VAC_OVP_7V (default)
                      charger.setVacOvp(BQ25798_VAC_OVP_26V) && charger.enableMppt(true) &&
                      charger.setMinimalSystemVoltage(BQ25798_VSYSMIN_MV(3000)) &&
                      configureThermalProfile();
      if (!ok) {
        chargerPresent   = false;
        chargerLostPolls = 0; // reset counter so timeout counts from now
        disableTsEmulation();
      }
    } else {
      disableTsEmulation();
    }

#ifdef INA3221_EMULATOR
    if (chargerPresent) {
      updateIna3221Registers(charger);
    } else if (!chargerEverSeen) {
      updateIna3221Ch3(kChargerNeverSeenBusMv, kChargerNeverSeenMa);
    } else if (++chargerLostPolls >= kChargerLostPollLimit) {
      updateIna3221Ch3(kChargerLostBusMv, kChargerLostMa);
      chargerLostPolls = kChargerLostPollLimit; // saturate so it keeps reporting
    }
    // else: charger lost < 10 s — hold last real values, no update
#endif
  }

sleep:
  // Sleep until next PIT tick (250 ms) or TWI address-match interrupt.
  sleep_mode();
}
