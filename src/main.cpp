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
//  Arduino  Port  Physical  Analog  Function
//      14  PA1      20      A1     SDA (INA3221 subordinate, Wire.swap(1))
//      15  PA2       1      A2     SCL (INA3221 subordinate, Wire.swap(1))
//      16~  PA3       2      A3     Chemistry select (direct ADC divider)
//       --  GND       3      --     GND
//       --  VDD       4      --     VDD
//       0~  PA4       5      A4     Main battery voltage (3:1 divider)
//       1~  PA5       6      A5     Backup battery voltage (3:1 divider)
//       2~  PA6       7      A6     TS emulation (DAC or Hi-Z)
//       3~  PA7       8      A7     Backup battery current (INA180 output)
//        4  PB5       9      A8     Supercap voltage (3:1 divider)
//        5  PB4      10      A9     Supercap bus voltage (3:1 divider)
//        6  PB3      11      --     Supercap MOSFET enable
//       7~  PB2      12      --     Supercap charge-rate control
//       8~  PB1      13     A10     SDA (BQ25798 master, SlowSoftI2CMaster)
//       9~  PB0      14     A11     SCL (BQ25798 master, SlowSoftI2CMaster)
//      10~  PC0      15      --     (unused)
//      11~  PC1      16      --     (unused)
//       12  PC2      17      --     (unused)
//       13  PC3      18      --     Load cutoff control
//       17  PA0      19      A0     UPDI (reserved for programming)
//       --   --      --      --     --

namespace {
constexpr uint8_t  kSubordinateI2cSdaPin  = PIN_WIRE_SDA_PINSWAP_1;
constexpr uint8_t  kSubordinateI2cSclPin  = PIN_WIRE_SCL_PINSWAP_1;
constexpr uint8_t  kChargerI2cSdaPin      = PIN_WIRE_SDA;
constexpr uint8_t  kChargerI2cSclPin      = PIN_WIRE_SCL;
constexpr uint8_t  kChemistrySelectPin    = A3;      // PA3, direct chemistry selector ladder
constexpr uint8_t  kMainBatterySensePin   = A4;      // PA4, 3:1 divider for main battery voltage
constexpr uint8_t  kBackupBatterySensePin = A5;      // PA5, 3:1 divider for backup battery voltage
constexpr uint8_t  kTsEmulationPin        = A6;      // PA6, DAC output / Hi-Z to BQ25798 TS pin
constexpr uint8_t  kBackupCurrentPin      = A7;      // PA7, INA180 current monitor output
constexpr uint8_t  kSupercapChargePin     = PIN_PB2; // PB2, charge-rate control
constexpr uint8_t  kSupercapMosfetPin     = PIN_PB3; // PB3, connects supercap path when charged
constexpr uint8_t  kSupercapBusSensePin   = PIN_PB4; // PB4, 3:1 divider for bus voltage
constexpr uint8_t  kSupercapCapSensePin   = PIN_PB5; // PB5, 3:1 divider for supercap voltage
constexpr uint8_t  kCutoffPin             = PIN_PC3; // PC3, load cutoff control
constexpr uint32_t I2CPollIntervalMs      = 2000;    // Poll charger every 2 seconds
constexpr uint32_t ADCPollIntervalMs      = 250;     // Poll battery voltage every 250ms
constexpr uint8_t  kSupercapFastChargePct = 90u;
constexpr uint8_t  kSupercapEnablePct     = 95u;
constexpr uint8_t  kTsWarmDacCode         = 192u; // <44% of 5V REGN = <88% of 2.5V DAC reference, set at 75% for safety

Bq25798 charger;
#ifdef BQ25798_USE_SOFT_I2C
SlowSoftI2CMaster chargerBus(kChargerI2cSdaPin, kChargerI2cSclPin, false);
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

// Write charge voltage and cutoff/reinstate thresholds to the charger for the current profile.
void configureBattery() {
  if (currentProfile == nullptr) {
    return;
  }
  charger.writeRegister16(BQ25798_REG_CHARGE_VOLTAGE_LIMIT, currentProfile->chargeReg);
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

// Read battery ADC and drive the cutoff pin with hysteresis.
void measureVoltage() {
  if (currentProfile == nullptr) {
    return;
  }
  const uint16_t voltage = readMainBatteryVoltage();
  if (voltage >= currentProfile->reinstateMv()) {
    // BatteryOK: normal operation
    if (cutoffActive) {
      cutoffActive = false;
    }
    digitalWrite(kCutoffPin, LOW);
  } else if (voltage < currentProfile->cutoffMv()) {
    // BatteryUVCO: disable load
    if (!cutoffActive) {
      cutoffActive = true;
    }
    digitalWrite(kCutoffPin, HIGH);
  }
  // BatteryLow: in hysteresis zone, do not change cutoff pin state
}

void updateDirectIna3221Channels() {
#ifdef INA3221_EMULATOR
  updateIna3221Ch1(readMainBatteryVoltage(), readMainBatteryCurrent());
  updateIna3221Ch2(readBackupBatteryVoltage(),
                   (int32_t)readBackupBatteryCurrent() * kShuntMilliohms);
#endif
}

void monitorSupercapCharging() {
  const uint16_t busMv = readSupercapBusVoltage();
  const uint16_t capMv = readSupercapVoltage();
  if (busMv == 0u) {
    pinMode(kSupercapChargePin, INPUT);
    digitalWrite(kSupercapMosfetPin, LOW);
    return;
  }

  const uint32_t capPct = static_cast<uint32_t>(capMv) * 100u;
  const uint32_t busPct = static_cast<uint32_t>(busMv);

  if (capPct >= static_cast<uint32_t>(kSupercapEnablePct) * busPct) {
    pinMode(kSupercapChargePin, OUTPUT);
    digitalWrite(kSupercapChargePin, LOW);
    digitalWrite(kSupercapMosfetPin, HIGH);
  } else {
    digitalWrite(kSupercapMosfetPin, LOW);
    if (capPct >= static_cast<uint32_t>(kSupercapFastChargePct) * busPct) {
      pinMode(kSupercapChargePin, OUTPUT);
      digitalWrite(kSupercapChargePin, LOW);
    } else {
      pinMode(kSupercapChargePin, INPUT);
    }
  }
}

} // namespace

void setup() {
  (void)kSubordinateI2cSdaPin;
  (void)kSubordinateI2cSclPin;

  pinMode(kCutoffPin, OUTPUT);
  digitalWrite(kCutoffPin, LOW);
  pinMode(kSupercapChargePin, INPUT); // HiZ until supercap precharge decides otherwise
  pinMode(kSupercapMosfetPin, OUTPUT);
  digitalWrite(kSupercapMosfetPin, LOW);
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

    // Supercap precharge and connect control
    monitorSupercapCharging();

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
      updateIna3221Ch3(1000u, 10); // 1 V / 10 mA — charger never seen
    } else if (++chargerLostPolls >= 5) {
      updateIna3221Ch3(2000u, 20); // 2 V / 20 mA — lost > 10 s (5 × 2000 ms)
      chargerLostPolls = 5;        // saturate so it keeps reporting
    }
    // else: charger lost < 10 s — hold last real values, no update
#endif
  }

sleep:
  // Sleep until next PIT tick (250 ms) or TWI address-match interrupt.
  sleep_mode();
}
