#pragma once

#include <Arduino.h>

// Battery chemistry profiles
enum class BatteryChemistry : uint8_t {
  TrueDefault = 0,  // Charger default - conservative/safe settings
  Highest = 1,
  LiionLL = 2,
  Liion = 3,
  Lifepo4 = 4,
  SodiumIon = 5,
  LTO = 6,
  NiMH3x = 7,
  NumProfiles,
};

// Compact battery profile — all fields fit in uint8_t to minimise flash.
struct BatteryProfile {
  uint8_t chargeReg;         // BQ25798 charge voltage register: (fullChargeMv - 2500) / 10
  uint8_t cutoffVoltage;     // mV / 25  (0 = no cutoff)
  uint8_t reinstateVoltage;  // mV / 25  (0 = disabled)

  uint16_t cutoffMv()    const { return (uint16_t)cutoffVoltage    * 25u; }
  uint16_t reinstateMv() const { return (uint16_t)reinstateVoltage * 25u; }
};

// Configuration table
namespace BatteryProfiles {

static constexpr uint8_t CHG(uint16_t mV)      { return (mV - 2500) / 10; }
static constexpr uint8_t CUTOFF(uint16_t mV)   { return mV / 25; }

constexpr BatteryProfile profiles[static_cast<uint8_t>(BatteryChemistry::NumProfiles)] = {
    {CHG(4100), CUTOFF(3000), CUTOFF(3200)},  // TrueDefault
    {CHG(4600), CUTOFF(   0), CUTOFF(   0)},  // Highest:   no cutoff
    {CHG(4100), CUTOFF(3100), CUTOFF(3400)},  // LiionLL
    {CHG(4200), CUTOFF(2900), CUTOFF(3100)},  // Liion
    {CHG(3650), CUTOFF(2500), CUTOFF(2700)},  // Lifepo4
    {CHG(3900), CUTOFF(1800), CUTOFF(2000)},  // SodiumIon
    {CHG(2900), CUTOFF(   0), CUTOFF(   0)},  // LTO:       no cutoff
    {CHG(4500), CUTOFF(2400), CUTOFF(2600)},  // NiMH3x
};

inline const BatteryProfile* getProfile(BatteryChemistry chemistry) {
  uint8_t index = static_cast<uint8_t>(chemistry);
  if (index >= static_cast<uint8_t>(BatteryChemistry::NumProfiles)) {
    return nullptr;
  }
  return &profiles[index];
}

// Select battery chemistry based on analog voltage level (0-8)
// Maps external selector (e.g., resistor divider, DIP switches) to chemistry
inline BatteryChemistry selectChemistryByLevel(uint8_t level) {
  // Level -> Chemistry mapping
  // 0-7 map directly to chemistries, level 8 and above default to TrueDefault
  if (level < static_cast<uint8_t>(BatteryChemistry::NumProfiles)) {
    return static_cast<BatteryChemistry>(level);
  }
  return BatteryChemistry::TrueDefault;  // Charger's safe default fallback
}

}  // namespace BatteryProfiles
