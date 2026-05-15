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

// Voltage settings for each chemistry (in millivolts)
struct BatteryProfile {
  uint16_t fullChargeVoltage;  // Charge complete threshold (mV)
  uint16_t cutoffVoltage;      // Minimum safe voltage; load cut off below this (mV)
  uint16_t reinstateVoltage;   // Voltage above which load is re-enabled (mV)
  BatteryChemistry chemistry;
};

// Configuration table
namespace BatteryProfiles {

constexpr BatteryProfile profiles[static_cast<uint8_t>(BatteryChemistry::NumProfiles)] = {
    // TrueDefault - Charger's safe default (conservative, LiionLL-like)
    {.fullChargeVoltage = 4100, .cutoffVoltage = 3000, .reinstateVoltage = 3200, .chemistry = BatteryChemistry::TrueDefault},
    // Highest
    {.fullChargeVoltage = 4600, .cutoffVoltage =    0, .reinstateVoltage =    0, .chemistry = BatteryChemistry::Highest},
    // LiionLL (Long Life variant)
    {.fullChargeVoltage = 4100, .cutoffVoltage = 3100, .reinstateVoltage = 3400, .chemistry = BatteryChemistry::LiionLL},
    // Standard Liion
    {.fullChargeVoltage = 4200, .cutoffVoltage = 2900, .reinstateVoltage = 3100, .chemistry = BatteryChemistry::Liion},
    // LiFePO4
    {.fullChargeVoltage = 3650, .cutoffVoltage = 2500, .reinstateVoltage = 2700, .chemistry = BatteryChemistry::Lifepo4},
    // Sodium Ion
    {.fullChargeVoltage = 3900, .cutoffVoltage = 1800, .reinstateVoltage = 2000, .chemistry = BatteryChemistry::SodiumIon},
    // LTO (Lithium Titanate Oxide)
    {.fullChargeVoltage = 2900, .cutoffVoltage =    0, .reinstateVoltage =    0, .chemistry = BatteryChemistry::LTO},
    // NiMH 3-cell
    {.fullChargeVoltage = 4500, .cutoffVoltage = 2400, .reinstateVoltage = 2600, .chemistry = BatteryChemistry::NiMH3x},
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
