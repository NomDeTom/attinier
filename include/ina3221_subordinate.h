#pragma once

#ifdef INA3221_EMULATOR

#include <Arduino.h>
#include <Wire.h>

#include "bq25798.h"
#include "bq25798_registers.h"

// ── INA3221 emulation ─────────────────────────────────────────────────────────
// The ATtiny presents itself on the I2C bus as an INA3221 3-channel
// current/voltage monitor (Texas Instruments, Manufacturer ID 0x5449).
//
//   Channel 1: Battery — VBAT (mV) / IBAT (mA)  from BQ25798 ADC
//   Channel 2: Aux     — V_aux (mV) / I_aux (mA) from ATtiny816 ADC (attiny816 build)
//              System  — VSYS (mV) / no current  from BQ25798 ADC   (other builds)
//   Channel 3: Input   — VBUS (mV) / IBUS (mA)  from BQ25798 ADC
//
// INA3221 subordinate address: 0x40 (A0=GND), 0x41 (A0=VS), 0x42 (A0=SDA), 0x43 (A0=SCL)
constexpr uint8_t kSubordinateI2cAddress = 0x42u; // default for meshtastic

// Shunt resistance assumed by this emulation (milliohms).
// The host INA3221 driver must be configured with the same value.
constexpr uint16_t kShuntMilliohms = 100u; // 100 mΩ

// Shadow of the INA3221 register bank.  Updated from the main loop;
// read by onSubordinateRequest() from the TWI ISR.
struct Ina3221Regs {
  uint16_t ch1Shunt = 0u;      // bits 15:3, 40 µV/LSB (signed)
  uint16_t ch1Bus   = 0u;      // bits 15:3,  8 mV/LSB
  uint16_t ch2Shunt = 0u;
  uint16_t ch2Bus   = 0u;
  uint16_t ch3Shunt = 0u;
  uint16_t ch3Bus   = 0u;
};
volatile Ina3221Regs ina3221;

volatile uint8_t ina3221RegPtr = 0x00u; // register pointer, set by master write

// Returns the big-endian 16-bit value for a given INA3221 register address.
inline uint16_t ina3221GetReg(uint8_t addr) {
  switch (addr) {
    case 0x00u: return 0x7127u; // config POR value: all 3 channels on, continuous
    case 0x01u: return ina3221.ch1Shunt;
    case 0x02u: return ina3221.ch1Bus;
    case 0x03u: return ina3221.ch2Shunt;
    case 0x04u: return ina3221.ch2Bus;
    case 0x05u: return ina3221.ch3Shunt;
    case 0x06u: return ina3221.ch3Bus;
    case 0xFEu: return 0x5449u; // Texas Instruments manufacturer ID
    case 0xFFu: return 0x3220u; // INA3221 die ID
    default:    return 0x0000u;
  }
}

// TWI subordinate receive: first byte written by the controller sets the register pointer.
inline void onSubordinateReceive(int numBytes) {
  if (numBytes >= 1 && Wire.available()) {
    ina3221RegPtr = Wire.read();
  }
  while (Wire.available()) (void)Wire.read(); // drain any trailing bytes
}

// TWI subordinate transmit: send the current register's 2 bytes (MSB first),
// then auto-increment the pointer for sequential reads.
inline void onSubordinateRequest() {
  const uint16_t val    = ina3221GetReg(ina3221RegPtr++);
  const uint8_t  buf[2] = { static_cast<uint8_t>(val >> 8),
                             static_cast<uint8_t>(val & 0xFFu) };
  Wire.write(buf, 2);
}

// Read a big-endian 16-bit ADC register from the BQ25798.
// (BQ25798 ADC: high byte at base address, low byte at base+1.)
inline bool readBq25798Adc(Bq25798& charger, uint8_t reg, uint16_t& out) {
  uint8_t hi = 0u, lo = 0u;
  if (!charger.readRegister8(reg, hi) || !charger.readRegister8(reg + 1u, lo)) {
    return false;
  }
  out = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

// Convert a BQ25798 current reading (mA, 1 mA/LSB) to an INA3221 shunt-voltage
// register value.  Negative values (discharge) are clamped to zero.
inline uint16_t toIna3221Shunt(int16_t currentMa) {
  if (currentMa < 0) currentMa = 0;
  // V_shunt (µV) = I (mA) × R (mΩ);  INA3221 shunt LSB = 40 µV, stored in bits 15:3.
  const uint32_t shuntUv = static_cast<uint32_t>(currentMa) * kShuntMilliohms;
  return static_cast<uint16_t>((shuntUv / 40u) << 3);
}

// Convert a BQ25798 voltage reading (mV, 1 mV/LSB) to an INA3221 bus-voltage
// register value (8 mV/LSB, stored in bits 15:3).
inline uint16_t toIna3221Bus(uint16_t voltageMv) {
  return static_cast<uint16_t>((voltageMv / 8u) << 3);
}

// Convert a shunt voltage in µV directly to an INA3221 shunt register value.
// Negative values (reverse current) are clamped to zero.
inline uint16_t toIna3221ShuntFromUv(int32_t shuntUv) {
  if (shuntUv < 0) shuntUv = 0;
  return static_cast<uint16_t>((static_cast<uint32_t>(shuntUv) / 40u) << 3);
}

// Update INA3221 channel 2 shadow registers from raw mV / µV measurements.
// busVoltMv: high-side voltage in mV; shuntUv: voltage across shunt in µV.
inline void updateIna3221Ch2(uint16_t busVoltMv, int32_t shuntUv) {
  const uint8_t sreg = SREG;
  cli();
  ina3221.ch2Bus   = toIna3221Bus(busVoltMv);
  ina3221.ch2Shunt = toIna3221ShuntFromUv(shuntUv);
  SREG = sreg;
}

// Populate all INA3221 shadow register channels with uniform fallback values.
// Used when BQ25798 is absent; voltageMv applies to all bus channels,
// currentMa to all shunt channels (ch2 shunt is always 0 — no system-side shunt).
inline void updateIna3221DummyValues(uint16_t voltageMv, int16_t currentMa) {
  const uint8_t sreg = SREG;
  cli();
  ina3221.ch1Bus   = toIna3221Bus(voltageMv);
  ina3221.ch1Shunt = toIna3221Shunt(currentMa);
#ifndef attiny816
  ina3221.ch2Bus   = toIna3221Bus(voltageMv);
  ina3221.ch2Shunt = 0u;
#endif
  ina3221.ch3Bus   = toIna3221Bus(voltageMv);
  ina3221.ch3Shunt = toIna3221Shunt(currentMa);
  SREG = sreg;
}

// Read BQ25798 ADC channels and refresh the INA3221 shadow registers.
// Call from the main loop only; never from ISR context.
inline void updateIna3221Registers(Bq25798& charger) {
  uint16_t vbus = 0u, vbat = 0u, vsys = 0u, ibusRaw = 0u, ibatRaw = 0u;
  readBq25798Adc(charger, BQ25798_REG_VBUS_ADC, vbus);
  readBq25798Adc(charger, BQ25798_REG_VBAT_ADC, vbat);
  readBq25798Adc(charger, BQ25798_REG_VSYS_ADC, vsys);
  readBq25798Adc(charger, BQ25798_REG_IBUS_ADC, ibusRaw);
  readBq25798Adc(charger, BQ25798_REG_IBAT_ADC, ibatRaw);

  // Guard against the TWI ISR reading a partially-updated struct.
  const uint8_t sreg = SREG;
  cli();
  ina3221.ch1Bus   = toIna3221Bus(vbat);
  ina3221.ch1Shunt = toIna3221Shunt(static_cast<int16_t>(ibatRaw));
#ifndef attiny816
  ina3221.ch2Bus   = toIna3221Bus(vsys);
  ina3221.ch2Shunt = 0u;
#endif
  ina3221.ch3Bus   = toIna3221Bus(vbus);
  ina3221.ch3Shunt = toIna3221Shunt(static_cast<int16_t>(ibusRaw));
  SREG = sreg;
}

#endif // INA3221_EMULATOR
