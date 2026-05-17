#pragma once

#include "bq25798_registers.h"
#include <Arduino.h>
#include <Wire.h>

class Bq25798 {
public:
  static constexpr uint8_t kDefaultAddress = BQ25798_DEFAULT_ADDR;

  enum class ChargePhase : uint8_t {
    NotCharging      = 0,
    TrickleCharge    = 1,
    PreCharge        = 2,
    FastCharge       = 3,
    TaperCharge      = 4,
    Reserved         = 5,
    TopOff           = 6,
    ChargeTerminated = 7,
  };

  struct Status {
    uint8_t     chargerStatus0 = 0;
    uint8_t     chargerStatus1 = 0;
    uint8_t     faultStatus0   = 0;
    uint8_t     faultStatus1   = 0;
    uint8_t     chargerFlag1   = 0;
    uint8_t     chargerFlag2   = 0;
    bool        powerGood      = false;
    bool        batteryPresent = false;
    bool        adcDone        = false;
    bool        faultActive    = false;
    ChargePhase phase          = ChargePhase::NotCharging;
  };

  bool begin(TwoWire &wire = Wire, uint8_t address = kDefaultAddress) {
    wire_    = &wire;
    address_ = address;
    // Verify part information register to confirm chip is present
    uint8_t part_info = 0;
    if (!readRegister8(BQ25798_REG_PART_INFORMATION, part_info)) {
      return false;
    }
    // Verify part number (bits 5-3 should be 011b = 3h for BQ25798)
    return (part_info & 0x38) == 0x18;
  }

  bool probe() const {
    if (wire_ == nullptr) {
      return false;
    }

    wire_->beginTransmission(address_);
    return wire_->endTransmission() == 0;
  }

  bool readRegister8(uint8_t reg, uint8_t &value) const {
    if (wire_ == nullptr) {
      return false;
    }

    wire_->beginTransmission(address_);
    wire_->write(reg);
    if (wire_->endTransmission(false) != 0) {
      return false;
    }

    if (wire_->requestFrom(address_, static_cast<uint8_t>(1)) != 1) {
      return false;
    }

    value = wire_->read();
    return true;
  }

  bool writeRegister8(uint8_t reg, uint8_t value) const {
    if (wire_ == nullptr) {
      return false;
    }

    wire_->beginTransmission(address_);
    wire_->write(reg);
    wire_->write(value);
    return wire_->endTransmission() == 0;
  }

  bool readRegister16(uint8_t reg, uint16_t &value) const {
    uint8_t low  = 0;
    uint8_t high = 0;
    if (!readRegister8(reg, low) || !readRegister8(reg + 1, high)) {
      return false;
    }

    value = static_cast<uint16_t>(low) | (static_cast<uint16_t>(high) << 8);
    return true;
  }

  bool writeRegister16(uint8_t reg, uint16_t value) const {
    if (wire_ == nullptr) {
      return false;
    }

    wire_->beginTransmission(address_);
    wire_->write(reg);
    wire_->write(static_cast<uint8_t>(value & 0xFF));
    wire_->write(static_cast<uint8_t>((value >> 8) & 0xFF));
    return wire_->endTransmission() == 0;
  }

  bool updateRegister8(uint8_t reg, uint8_t mask, uint8_t value) const {
    uint8_t current = 0;
    if (!readRegister8(reg, current)) {
      return false;
    }

    current = static_cast<uint8_t>((current & ~mask) | (value & mask));
    return writeRegister8(reg, current);
  }

  bool enableCharging(bool enabled) const {
    // Bit 5 of CHARGER_CONTROL_0 enables/disables charging
    return updateRegister8(BQ25798_REG_CHARGER_CONTROL_0, 1u << 5,
                           enabled ? static_cast<uint8_t>(1u << 5) : 0u);
  }

  bool setHighImpedance(bool enabled) const {
    // Bit 2 of CHARGER_CONTROL_0 controls HIZ mode
    return updateRegister8(BQ25798_REG_CHARGER_CONTROL_0, 1u << 2,
                           enabled ? static_cast<uint8_t>(1u << 2) : 0u);
  }

  bool enableAdc(bool enabled) const {
    return updateRegister8(BQ25798_REG_ADC_CONTROL, 1u << 7,
                           enabled ? static_cast<uint8_t>(1u << 7) : 0u);
  }

  bool disableWatchdog() const {
    // Bits 2:0 of CHARGER_CONTROL_1 control watchdog timer
    // Set to 0x00 to disable
    return updateRegister8(BQ25798_REG_CHARGER_CONTROL_1, 0x07, 0x00);
  }

  bool setVacOvp(bq25798_vac_ovp_t level) const {
    // Bits 7:6 of CHARGER_CONTROL_1 set VAC overvoltage protection threshold
    return updateRegister8(BQ25798_REG_CHARGER_CONTROL_1, 0xC0, static_cast<uint8_t>(level) << 6);
  }

  bool enableMppt(bool enabled) const {
    // Bit 0 of MPPT_CONTROL enables/disables MPPT function
    return updateRegister8(BQ25798_REG_MPPT_CONTROL, BQ25798_MPPT_EN_BIT,
                           enabled ? BQ25798_MPPT_EN_BIT : 0u);
  }

  bool setMinimalSystemVoltage(uint8_t value) const {
    // Set VSYSMIN (bits 5:0 of REG00) only if different.
    uint8_t current = 0;
    if (!readRegister8(BQ25798_REG_MINIMAL_SYSTEM_VOLTAGE, current)) {
      return false;
    }
    // Check if bits 5:0 already match (ignore bits 7:6 which are reserved).
    if ((current & 0x3F) == (value & 0x3F)) {
      return true; // Already set correctly.
    }
    // Update: preserve reserved bits 7:6, write new bits 5:0.
    return writeRegister8(BQ25798_REG_MINIMAL_SYSTEM_VOLTAGE, (current & 0xC0) | (value & 0x3F));
  }

  bool readStatus(Status &status) const {
    if (!readRegister8(BQ25798_REG_CHARGER_STATUS_0, status.chargerStatus0) ||
        !readRegister8(BQ25798_REG_CHARGER_STATUS_1, status.chargerStatus1) ||
        !readRegister8(BQ25798_REG_FAULT_STATUS_0, status.faultStatus0) ||
        !readRegister8(BQ25798_REG_FAULT_STATUS_1, status.faultStatus1) ||
        !readRegister8(BQ25798_REG_CHARGER_FLAG_1, status.chargerFlag1) ||
        !readRegister8(BQ25798_REG_CHARGER_FLAG_2, status.chargerFlag2)) {
      return false;
    }

    status.powerGood      = ((status.chargerStatus0 >> 3) & 0x01u) != 0;
    status.phase          = static_cast<ChargePhase>((status.chargerStatus1 >> 5) & 0x07u);
    status.batteryPresent = ((status.chargerFlag1 >> 1) & 0x01u) != 0;
    status.adcDone        = ((status.chargerFlag2 >> 5) & 0x01u) != 0;
    status.faultActive    = (status.faultStatus0 != 0) || (status.faultStatus1 != 0);
    return true;
  }

private:
  TwoWire *wire_    = nullptr;
  uint8_t  address_ = kDefaultAddress;
};