# attinier

ATtiny412-based battery charger controller using the BQ25798.

## BQ25798 driver

The project uses a minimal I2C driver in `include/bq25798.h` rather than the full Adafruit BQ25798 library.

The Adafruit library was evaluated but a direct PlatformIO integration overflowed ATtiny412 flash by ~1981 bytes. The current firmware only needs a small subset of the register interface:

- `begin()` — probe and verify the part number
- `writeRegister8()` / `writeRegister16()` — write configuration registers
- `updateRegister8()` — read-modify-write for single-bit fields
- `disableWatchdog()` — clear watchdog timer bits
- `enableCharging()` / `enableAdc()` / `setHighImpedance()` — charger control

The upstream library is at <https://github.com/adafruit/Adafruit_bq25798> and can be used as a reference for register definitions.

## Hardware

### ATtiny412 pinmap (SOIC-8)

```
              ┌────────┐
   VCC    1 ──┤        ├── 8   PA0  UPDI (programming only)
   PA6    2 ──┤        ├── 7   GND
   PA7    3 ──┤        ├── 6   PA3
   PA1    4 ──┤        ├── 5   PA2
              └────────┘
```

| Arduino | Port | Physical | Analog | Function            |
|---------|------|----------|--------|---------------------|
| 0       | PA0  | 8        | A0     | UPDI (do not use)   |
| 1       | PA1  | 4        | A1     | SDA (Wire)          |
| 2       | PA2  | 5        | A2     | SCL (Wire)          |
| 3       | PA3  | 6        | A3     | Chemistry select    |
| 6       | PA6  | 2        | A6     | Battery voltage ADC |
| 7       | PA7  | 3        | A7     | Load cutoff output  |

Wiring notes:
- Add external pull-ups on SDA and SCL (`4.7 kΩ` to the logic rail).
- Keep BQ25798 and ATtiny412 on the same I2C voltage domain.
- Leave PA0 free for UPDI flashing at all times.

## Firmware overview

The main loop runs every 250 ms and follows this sequence:

1. **ReadChemistry** — reads the analog voltage divider on PA3 and maps it to one of 8 battery chemistry profiles.
2. **ConfigureBat** — on chemistry change, writes the new charge voltage limit to BQ25798 `REG01` (CHARGE_VOLTAGE_LIMIT).
3. **MeasureVoltage** — reads VCC via the internal 1.1 V bandgap, then reads the battery voltage on PA6 and drives the load cutoff pin on PA7 with hysteresis:
   - Voltage ≥ reinstate → PA7 LOW (load on, normal operation)
   - Voltage < cutoff → PA7 HIGH (load off, UVCO)
   - Between cutoff and reinstate → no change (hysteresis zone)
4. **CheckCharger** — re-disables the BQ25798 watchdog timer to prevent register resets.

Setup additionally sets the BQ25798 minimal system voltage to 3 V (`REG00 = 0x02`).

### Chemistry profiles

The chemistry is selected by the analog level on PA3 (0–7). All voltages are in mV.

| # | Chemistry   | Charge V | Cutoff | Reinstate |
|---|-------------|----------|--------|-----------|
| 0 | TrueDefault | 4100     | 3000   | 3200      |
| 1 | Highest     | 4600     | —      | —         |
| 2 | LiionLL     | 4100     | 3100   | 3400      |
| 3 | Liion       | 4200     | 2900   | 3100      |
| 4 | LiFePO4     | 3650     | 2500   | 2700      |
| 5 | Sodium-ion  | 3900     | 1800   | 2000      |
| 6 | LTO         | 2900     | —      | —         |
| 7 | NiMH 3×     | 4500     | 2400   | 2600      |

Profiles with no cutoff (Highest, LTO) never assert the cutoff pin.

## Flashing

The board uses `jtag2updi`. `platformio.ini` sets this as the default upload protocol.

### Connections

| ATtiny412 | Programmer  |
|-----------|-------------|
| VCC       | VCC (if programmer supplies target reference) |
| GND       | GND         |
| PA0       | UPDI        |

Do not connect the programmer to SDA or SCL.

### PlatformIO commands

```powershell
# Build
platformio run -e ATtiny412

# Upload
platformio run -e ATtiny412 -t upload

# Clean
platformio run -e ATtiny412 -t clean
```

If your jtag2updi adapter appears as a serial port, uncomment and set in `platformio.ini`:

```ini
upload_port = COM4
```

### First flash checklist

1. Confirm supply voltage and SDA/SCL pull-ups are present.
2. Confirm PA0 is only connected to the UPDI programmer.
3. Build, then upload.
4. If upload fails, verify the COM port and jtag2updi wiring.


