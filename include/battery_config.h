#pragma once

#include <Arduino.h>

// Battery chemistry profiles
enum class BatteryChemistry : uint8_t {
  TrueDefault        = 0, // Charger default - conservative/safe settings
  SupercapacitorBank = 1,
  LiionLL            = 2,
  Liion              = 3,
  Lifepo4            = 4,
  SodiumIon3p9       = 5,
  SodiumIon4p1       = 6,
  LTO                = 7,
  NiMH3x             = 8,
  Reserved8          = 9, // placeholder
  NumProfiles,
};

enum class LowVPreCharge : uint8_t {
  Reduced = 0,
  Full,
};

// Compact battery profile — all fields fit in uint8_t to minimise flash.
struct BatteryProfile {
  uint8_t       chargeReg;        // BQ25798 charge voltage register: (fullChargeMv - 2500) / 10
  uint8_t       cutoffVoltage;    // mV / 25  (0 = no cutoff)
  uint8_t       reinstateVoltage; // mV / 25  (0 = disabled)
  LowVPreCharge lowVoltageChargePolicy;

  uint16_t cutoffMv() const {
    return (uint16_t)cutoffVoltage * 25u;
  }
  uint16_t reinstateMv() const {
    return (uint16_t)reinstateVoltage * 25u;
  }
};

// Configuration table
namespace BatteryProfiles {

static constexpr uint8_t chargeReg(uint16_t mV) {
  return (mV - 2500) / 10;
}
static constexpr uint8_t cutoffReg(uint16_t mV) {
  return mV / 25;
}
static constexpr uint8_t reinstateFrom(uint16_t mV) {
  return mV / 25;
}

constexpr BatteryProfile profiles[static_cast<uint8_t>(BatteryChemistry::NumProfiles)] = {
    {chargeReg(4100), cutoffReg(3000), reinstateFrom(3200), LowVPreCharge::Reduced}, // TrueDefault
    {chargeReg(4600), cutoffReg(1500), reinstateFrom(2000), LowVPreCharge::Full}, // Supercapacitor
    {chargeReg(4100), cutoffReg(3100), reinstateFrom(3400), LowVPreCharge::Reduced}, // LiionLL
    {chargeReg(4200), cutoffReg(2900), reinstateFrom(3100), LowVPreCharge::Reduced}, // Liion
    {chargeReg(3650), cutoffReg(2500), reinstateFrom(2700), LowVPreCharge::Reduced}, // Lifepo4
    {chargeReg(3900), cutoffReg(1500), reinstateFrom(2000), LowVPreCharge::Reduced}, // SodiumIon3.9
    {chargeReg(4100), cutoffReg(1500), reinstateFrom(2000), LowVPreCharge::Reduced}, // SodiumIon4.1
    {chargeReg(2800), cutoffReg(1500), reinstateFrom(2000), LowVPreCharge::Full},    // LTO
    {chargeReg(4500), cutoffReg(2400), reinstateFrom(2600), LowVPreCharge::Reduced}, // NiMH3x
    {chargeReg(4100), cutoffReg(3000), reinstateFrom(3200), LowVPreCharge::Reduced}, // Reserved8
};

inline const BatteryProfile *getProfile(BatteryChemistry chemistry) {
  uint8_t index = static_cast<uint8_t>(chemistry);
  if (index >= static_cast<uint8_t>(BatteryChemistry::NumProfiles)) {
    return nullptr;
  }
  return &profiles[index];
}

// Select battery chemistry based on analog voltage level (0-9)
// Maps external selector (e.g., resistor divider, DIP switches) to chemistry
inline BatteryChemistry selectChemistryByLevel(uint8_t level) {
  // Levels 0-9 map directly to chemistry indices; anything above defaults to TrueDefault
  if (level < static_cast<uint8_t>(BatteryChemistry::NumProfiles)) {
    return static_cast<BatteryChemistry>(level);
  }
  return BatteryChemistry::TrueDefault; // Charger's safe default fallback
}

} // namespace BatteryProfiles
