# attinier

ATtiny-based battery charger controller using the BQ25798. Three target builds are supported:

| Environment | Package | Flash | RAM   | Notes                                               |
| ----------- | ------- | ----- | ----- | --------------------------------------------------- |
| ATtiny412   | SOIC-8  | 4 KB  | 256 B | Minimal — cutoff + charger                          |
| ATtiny816   | VQFN-20 | 8 KB  | 512 B | + INA3221 emulation, softstart, aux ADC             |
| ATtiny1616  | VQFN-20 | 16 KB | 2 KB  | Same firmware as ATtiny816 without `attiny816` flag |

All builds run at **4 MHz** (`board_build.f_cpu = 4000000L`) and use **STANDBY sleep** with an RTC PIT for wakeup timing.

---

## BQ25798 driver

A minimal I²C driver lives in `include/bq25798.h`. The full Adafruit BQ25798 library overflows ATtiny412 flash by ~1981 bytes; this driver exposes only the subset the firmware needs:

- `begin()` — probe and verify the part number
- `writeRegister8()` / `writeRegister16()` — write configuration registers
- `updateRegister8()` — read-modify-write for single-bit fields
- `disableWatchdog()` — clear watchdog timer bits
- `enableCharging()` / `enableAdc()` / `setHighImpedance()` — charger control
- `setVacOvp()` / `enableMppt()` / `setMinimalSystemVoltage()` — protection / MPPT

Upstream library for reference: <https://github.com/adafruit/Adafruit_bq25798>

---

## Hardware

### ATtiny412 pinmap (SOIC-8)

```
                      ┌────────┐
           VCC    1 ──┤        ├── 8   GND
DAC/AIN6   PA6    2 ──┤        ├── 7   PA3   SCK/CLKI/AIN3
    AIN7   PA7    3 ──┤        ├── 6   PA0   UPDI (programming only)
SDA/AIN1   PA1    4 ──┤        ├── 5   PA2   SCL/AIN2
                      └────────┘
```

| Arduino | Port | Physical | Analog | Function            |
| ------- | ---- | -------- | ------ | ------------------- |
| 0~      | PA6  | 2        | A6     | Battery voltage ADC |
| 1~      | PA7  | 3        | A7     | Load cutoff output  |
| 2~      | PA1  | 4        | A1     | SDA (Wire)          |
| 3~      | PA2  | 5        | A2     | SCL (Wire)          |
| 4~      | PA3  | 7        | A3     | Chemistry select    |
| 5       | PA0  | 6        | A0     | UPDI (do not use)   |

Wiring notes:

- Add external pull-ups on SDA and SCL (4.7 kΩ to the logic rail).
- Keep BQ25798 and ATtiny on the same I²C voltage domain.
- Leave PA0 free for UPDI flashing at all times.

---

### ATtiny816 pinmap (VQFN-20)

```
           +----------T20 PA1  SDA_alt  (active TWI SDA via Wire.swap(1))
           | +--------T19 PA0  UPDI (reserved for programming)
           | | +------T18 PC3
           | | |  +---T17 PC2
           | | |  | +-T16 PC1
           | | |  | |
         +-+-+-+--+-+-+
 1 PA2 --|             |-- PC0 15
 2 PA3 --|  ATtiny816  |-- PB0 14
 3 GND --|   VQFN-20   |-- PB1 13
 4 VDD --|             |-- PB2 12
 5 PA4 --|             |-- PB3 11
         +-+-+-+--+-+-+
           | | |  | |
           | | |  | +-B10 PB4
           | | |  +---B 9 PB5
           | | +------B 8 PA7
           | +--------B 7 PA6
           +----------B 6 PA5
```

| Arduino | Port | Physical | Analog | Function                                     |
| ------- | ---- | -------- | ------ | -------------------------------------------- |
| 15      | PA2  | 1        | A2     | SCL_alt — active TWI SCL via `Wire.swap(1)`  |
| 16~     | PA3  | 2        | A3     | Chemistry select                             |
| —       | GND  | 3        | —      | Ground                                       |
| —       | VDD  | 4        | —      | Supply                                       |
| 0~      | PA4  | 5        | A4     | (spare)                                      |
| 1~      | PA5  | 6        | A5     | VREFA                                        |
| 2~      | PA6  | 7        | A6     | Aux bus voltage ADC (INA3221 ch2 bus)        |
| 3~      | PA7  | 8        | A7     | Aux shunt low-side ADC (INA3221 ch2 shunt)   |
| 4       | PB5  | 9        | A8     | Softstart control (OUTPUT LOW / INPUT HiZ)   |
| 5       | PB4  | 10       | A9     | Softstart monitor ADC                        |
| 6       | PB3  | 11       | —      | Bus monitor ADC (for softstart comparator)   |
| 7~      | PB2  | 12       | —      | (spare / Output enable — unused)             |
| 8~      | PB1  | 13       | A10    | SDA Wire default (inactive — swap(1) in use) |
| 9~      | PB0  | 14       | A11    | SCL Wire default (inactive — swap(1) in use) |
| 10~     | PC0  | 15       | —      | (spare)                                      |
| 11~     | PC1  | 16       | —      | (spare)                                      |
| 12      | PC2  | 17       | —      | (spare)                                      |
| 13      | PC3  | 18       | —      | (spare)                                      |
| 17      | PA0  | 19       | A0     | UPDI (reserved for programming)              |
| 14      | PA1  | 20       | A1     | SDA_alt — active TWI SDA via `Wire.swap(1)`  |

`Wire.swap(1)` routes the TWI peripheral to PA1/PA2 (physical pins 20/1), freeing PB0/PB1 for PWM.

---

## Firmware overview

### Timing and sleep

The firmware uses **STANDBY sleep** with the **RTC Periodic Interrupt Timer** for wakeup. The PIT runs from the internal `OSCULP32K` oscillator (32.768 kHz), which stays active in STANDBY without the main 4 MHz clock.

- PIT period: `CYC8192` = 8192 cycles = exactly **250 ms**
- ADC poll: every PIT wakeup (**250 ms**)
- I²C poll: every 8th PIT wakeup (**2000 ms**)

The TWI address-match interrupt also wakes the CPU from STANDBY, so the INA3221 slave responds to I²C requests immediately without polling overhead.

### Main loop sequence

Each 250 ms ADC tick:

1. **ReadChemistry** — reads the analog divider on PA3 (ATtiny412) or PA3/pin 2 (ATtiny816) and maps to one of 8 battery chemistry profiles. On change, writes the new charge voltage to BQ25798 `REG01`.
2. **MeasureVoltage** — reads VCC via the internal 1.1 V bandgap, then reads battery voltage and drives the cutoff pin with hysteresis:
   - voltage ≥ reinstate → cutoff LOW (load on)
   - voltage < cutoff → cutoff HIGH (load off, UVCO)
   - in hysteresis zone → no change
   - On ATtiny816: the softstart pin is set to HiZ (INPUT) on both cutoff activation and release.
3. **MonitorSoftStartPin** _(ATtiny816 only)_ — compares softstart monitor (PB4) and bus monitor (PB3) voltages. If the softstart monitor lags >10 mV behind the bus monitor, PB5 is set to INPUT (HiZ) to let the softstart circuit ramp; otherwise PB5 is driven LOW.
4. **MeasureAuxChannel** _(ATtiny816 + INA3221_EMULATOR only)_ — measures PA6 (bus) and PA6/PA7 differential (64-pair oversampled) and updates INA3221 channel 2 shadow registers.

Each 2000 ms I²C tick (every 8th ADC tick):

5. **ProbeCharger** — if charger was lost, re-runs `begin()` (reads part ID, not just ACK) to re-detect.
6. **CheckCharger** — disables BQ25798 watchdog, sets VAC OVP to 26 V, enables MPPT, sets minimum system voltage to 3 V.
7. **UpdateINA3221** _(INA3221_EMULATOR only)_ — reads BQ25798 ADC registers (VBUS, IBUS, VBAT, IBAT, VSYS) and updates the INA3221 slave register shadow. On charger loss, holds the last real values for up to 10 s (5 polls), then substitutes dummy values.

### Chemistry profiles

Selected by the analog level on the chemistry pin (0–7). All voltages in mV.

| #   | Chemistry   | Charge V | Cutoff | Reinstate |
| --- | ----------- | -------- | ------ | --------- |
| 0   | TrueDefault | 4100     | 3000   | 3200      |
| 1   | Highest     | 4600     | —      | —         |
| 2   | LiionLL     | 4100     | 3100   | 3400      |
| 3   | Liion       | 4200     | 2900   | 3100      |
| 4   | LiFePO4     | 3650     | 2500   | 2700      |
| 5   | Sodium-ion  | 3900     | 1800   | 2000      |
| 6   | LTO         | 2900     | —      | —         |
| 7   | NiMH 3×     | 4500     | 2400   | 2600      |

Profiles without cutoff (Highest, LTO) never assert the cutoff pin.

---

## INA3221 emulation (`INA3221_EMULATOR`)

When built with `-DINA3221_EMULATOR`, the firmware acts as an I²C slave at the INA3221 address and emulates three measurement channels:

| Channel   | Content                                    |
| --------- | ------------------------------------------ |
| 1         | BQ25798 VBUS / IBUS                        |
| 2 (816)   | Aux bus/shunt from PA6/PA7 ADC (ATtiny816) |
| 2 (other) | BQ25798 VSYS                               |
| 3         | BQ25798 VBAT / IBAT                        |

The slave uses the megaTinyCore TWI MANDS (master-and-slave) dual mode (`-DTWI_MANDS`), allowing the same peripheral to act as both an I²C master (to talk to BQ25798) and a slave (to respond to the host).

---

## Build sizes (4 MHz, current firmware)

| Environment | Flash used     | Flash % | RAM used     | RAM % |
| ----------- | -------------- | ------- | ------------ | ----- |
| ATtiny412   | 3814 / 4096 B  | 93 %    | 157 / 256 B  | 61 %  |
| ATtiny816   | 4440 / 8192 B  | 54 %    | 157 / 512 B  | 31 %  |
| ATtiny1616  | 3988 / 16384 B | 24 %    | 157 / 2048 B | 8 %   |

---

## Flashing

All environments use **serialupdi** at 57600 baud. A USB-to-UART adapter with a 1 kΩ series resistor on the TX line is sufficient.

### Connections

| ATtiny pin | Signal | Programmer                            |
| ---------- | ------ | ------------------------------------- |
| VCC        | VCC    | optional — may supply from programmer |
| GND        | GND    | GND                                   |
| PA0        | UPDI   | TX (via 1 kΩ)                         |

Do not connect the programmer to SDA, SCL, or any other signal pin.

### PlatformIO commands

```powershell
# Build all
platformio run

# Build one target
platformio run -e ATtiny412
platformio run -e ATtiny816
platformio run -e ATtiny1616

# Upload
platformio run -e ATtiny816 -t upload
```

Set the correct COM port in `platformio.ini` by uncommenting:

```ini
upload_port = COM4
```

### First flash checklist

1. Confirm supply voltage and SDA/SCL pull-ups are present.
2. Confirm PA0 is connected only to the UPDI line.
3. Build, then upload.
4. If upload fails, verify the COM port, adapter wiring, and the 1 kΩ series resistor.
