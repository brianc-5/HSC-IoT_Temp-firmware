# Hardware and wiring

## Required hardware

For the smallest working system:

- two Seeed Studio XIAO nRF52840 boards;
- two data-capable USB-C cables;
- a Chrome-on-Android device for the Web Bluetooth application.

Optional sensor-side hardware:

- [XIAO Logger HAT V3](https://github.com/potblitd/XIAO-log) for ambient
  temperature, humidity, illuminance, RTC hardware, and a gated voltage
  divider;
- indoor photovoltaic source, storage capacitor/supercapacitor, and a
  BQ25570-based harvester such as a CJMCU module;
- Nordic PPK2 or an equivalent current profiler and a calibrated multimeter.

The project has no approved stock harvester assembly. Select and verify every
energy-path component for the actual source, storage voltage, load, and
environment.

## Board assignment

Mark the boards physically before flashing:

| Board | Firmware | Normal power |
| --- | --- | --- |
| Sensor | `HSC-Tplus-sensor-zephyr.uf2` | External/harvested rail; USB only for setup |
| Bridge | `HSC-Tplus-bridge-commissioning.uf2` or an address-locked build | Continuous USB |

Both files target the same MCU and bootloader, so the bootloader cannot prevent
you from installing the wrong role.

## Logger HAT V3 production mapping

The V3 schematic—not older prose in the upstream README—is authoritative for
the voltage-divider pin.

| HAT function | XIAO pin | nRF52840 resource | Firmware use |
| --- | --- | --- | --- |
| Shared sensor/divider enable | D10 | P1.15 | Active high around a HAT measurement |
| I²C SDA | D4 | P0.04 / `i2c1` | SHT40, BH1750, PCF8563 bus |
| I²C SCL | D5 | P0.05 / `i2c1` | SHT40, BH1750, PCF8563 bus |
| Divider ADC | D1 / A1 | P0.03 / SAADC AIN1 | VSTOR measurement |
| RTC interrupt | D0 / A0 | GPIO | Not used by current firmware |
| Ground | GND | Ground | Common reference |
| HAT logic/RTC rail | 3V3 | Supply | Always connected in a direct stack |

HAT devices:

- SHT40 temperature/humidity sensor at `0x44`;
- BH1750FVI light sensor at `0x23` because ADDR is grounded;
- PCF8563 RTC at `0x51` with a 32.768 kHz crystal;
- two 10 kΩ I²C pull-ups tied to HAT 3V3;
- 100 kΩ / 100 kΩ gated divider with 100 nF at the ADC node.

The current firmware does not use the PCF8563 or its interrupt. The bridge/web
app provide wall time, and Zephyr's internal timer schedules the 20-second
wake interval.

## Direct-stack HAT configuration

The functionally validated configuration is a normal HAT stack:

1. Align pin labels and orientation before insertion.
2. Pass 3V3, GND, D10, D4, D5, D1/A1, and D0 normally.
3. If VSTOR is to be measured, connect the HAT `BAT+` input to the storage node
   only after confirming its voltage is within the divider/ADC design range.
4. Flash the release Zephyr sensor image. It detects the HAT automatically.

The SHT40/BH1750/divider rail is switched by D10. The PCF8563 and I²C pull-ups
remain on 3V3. This arrangement works functionally but has shown higher than
expected standby draw. Do not publish an average-current claim until you have
measured your complete assembly. See [HAT_POWER.md](HAT_POWER.md).

## Voltage-divider behavior

The two equal 100 kΩ resistors produce:

```text
VADC = VSTOR / 2
```

The firmware uses AIN1 with the internal 0.6 V reference, gain 1/4, 12-bit
resolution, 40 µs acquisition, and 64× oversampling. It waits at least 25 ms
after D10 rises before sampling the 100 nF node. It accepts VSTOR only in the
configurable 1.5–4.5 V plausibility band.

Validate before use:

1. Disconnect USB from the sensor.
2. Apply three known storage-node voltages covering the intended range.
3. Measure VSTOR and A1 with a high-impedance multimeter.
4. Confirm `VSTOR / VADC` is close to 2.0.
5. Compare each advertised voltage with the meter.
6. Reject the assembly if the ADC pin can exceed the MCU's permitted input.

When USB is present, HAT voltage is deliberately advertised as invalid so
bench power does not enter harvested-power calculations.

## External power

The sensor external supply must be compatible with the XIAO and every attached
device. Confirm polarity and voltage under no load and during the radio burst.
The CJMCU/BQ25570 board may expose nodes named INPUT, BAT/VSTOR, and OUTPUT, but
clone boards and resistor populations differ. Measure them; do not infer their
settings from silkscreen or a marketplace listing.

> Never connect the sensor USB port and an external 3.3 V output together
> unless you have explicitly isolated the two sources. Firmware VBUS detection
> is not a power multiplexer.

See [ENERGY_HARVESTING.md](ENERGY_HARVESTING.md) for the prototype power path.

## Bring-up instruments

A robust bring-up uses:

- multimeter for polarity, rail voltages, and divider ratio;
- oscilloscope or logic analyzer for D10 and I²C timing;
- current profiler for sleep floor, measurement peak, and cycle average;
- nRF Connect or a BLE packet capture for exact advertisement contents;
- USB serial monitor at 115200 baud for bridge commissioning and diagnostics.

Record board revisions, resistor values, capacitor value/leakage, firmware tag,
ambient light, and instrument setup with every power measurement. Without those
details, current figures are not comparable.
