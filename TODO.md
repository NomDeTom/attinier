# ATtiny816 Feature Checklist

## Completed

- [x] Direct sampling of the chemistry select (PA3, direct ADC divider)
- [x] Direct sampling of main and backup battery voltage as INA3221 channels 1 and 2 (PA4/PA5, 3:1 dividers)
- [x] Backup battery current sampling via INA180 helper (PA7)
- [x] Two pins monitoring supercap charging (PB4/PB5 monitors, PB2 charge-rate control, PB3 MOSFET)
- [x] Open P-MOSFET when supercap voltage within 95% of bus voltage (PB3, active-low gate drive: LOW = enable, Hi-Z = disable)
- [x] High-impedance pin to BQ25798 TS input with warm-zone emulation for LTO (PA6 DAC, disableTsEmulation/enableTsWarmEmulation functions)
- [x] 100 mV reduction in target voltage for LTO chemistry (BQ25798_JEITA_VSET_M100MV in configureThermalProfile)

## Pending

- [ ] SSD1306 status display on the BQ25798 I2C bus
  - Sharing the existing PB1/PB0 SlowSoftI2CMaster bus is feasible; a typical SSD1306 address (0x3C/0x3D) does not conflict with the BQ25798 charger address.
  - The main constraint is library size and whether the display driver can be adapted to the existing SlowSoftI2CMaster bus instead of assuming hardware Wire.
  - Isolated ATtiny816 benchmark results at 4 MHz:
  - Empty sketch: 294 flash / 10 RAM
  - SSD1306Ascii: 2740 flash / 86 RAM
  - U8g2 U8x8 text mode: 5031 flash / 161 RAM
  - Adafruit_SSD1306: not suitable here; the full graphics stack is too heavy for this target.
  - Recommended path: use SSD1306Ascii or a tiny custom text-only SSD1306 driver layered on the existing chargerBus.
  - Avoid U8g2 and Adafruit_SSD1306 on this 8 KB target unless other features are removed to recover substantial flash.
  - If implemented, keep the UI to simple rotating text screens from the main loop rather than a buffered graphics interface.
- [x] Fast-charge to 4.5V supercap chemistry option
  - SupercapacitorBank profile (level 1) charges to 4.6 V with LowVPreCharge::Full; covers the use case
  - Chemistry table expanded to 10 slots (levels 8 and 9 are Reserved placeholders for future use)
- [ ] Soft-start for 3.3V and 5V DC converters once supercap is fully charged
  - Define soft-start enable pin(s) for each converter
  - Add soft-start logic to trigger after supercap reaches 95% threshold
  - Sequence converter startup to avoid inrush current
- [x] Test switch input (active low) that drives a complementary output pin high
  - PC0 (kTestSwitchPin) configured as input with pullup; LOW = switch pressed
  - PC1 (kTestIndicatorPin) driven HIGH when switch is pressed, LOW otherwise
  - 250 ms ADC poll period provides sufficient debounce for a test switch
  - Integrated into main ADC poll loop
