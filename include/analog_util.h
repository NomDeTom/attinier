#pragma once

#include <Arduino.h>

// Read an analog pin and return voltage as a 0-7 scale of VCC
// Example: if VCC = 5V and pin reads 2.5V, returns 3-4 (representing ~2.5V / 5V * 7)
inline uint8_t readVoltageLevel(uint8_t pin) {
  // Read ADC value (0-1023 for 10-bit ADC)
  uint16_t adcValue = analogRead(pin);
  
  // Scale to 0-7 range: (adcValue / 1023) * 7
  // Avoid floating point: (adcValue * 7) / 1023
  // Add 512 for rounding instead of truncating
  uint8_t level = (adcValue * 7 + 512) / 1023;
  
  // Clamp to 0-7 range (rounding might push to 8)
  if (level > 7) {
    level = 7;
  }
  
  return level;
}

// Read an analog pin and return the voltage in millivolts.
// vccMillivolts: supply voltage in mV (must match the ADC reference).
inline uint16_t readVoltage(uint8_t pin, uint16_t vccMillivolts) {
  uint16_t adcValue = analogRead(pin);
  return static_cast<uint16_t>((static_cast<uint32_t>(adcValue) * vccMillivolts) / 1023u);
}

// Measure VCC by reading the 1.1V internal bandgap reference with VCC as the ADC reference.
// Accuracy is limited by bandgap tolerance (~±10%), sufficient for voltage threshold comparisons.
inline uint16_t readVcc() {
  analogReference(INTERNAL1V1);  // configure VREF module to 1.1V
  analogReference(VDD);          // switch ADC reference back to VDD
  uint16_t raw = analogRead(ADC_INTREF);
  if (raw == 0) return 3300;     // fallback if ADC reads zero
  return static_cast<uint16_t>(1100UL * 1023UL / raw);
}
