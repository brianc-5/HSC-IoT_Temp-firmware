# XIAO nRF52840 Protocol v3 sensor

This Zephyr application is a low-power, non-connectable BTHome v2 broadcaster
for the Seeed Studio XIAO nRF52840. One firmware image automatically supports a
bare XIAO or a Logger HAT V3 and identifies USB versus external power in every
new sample.

The Logger HAT path has been functionally validated on target hardware: its
temperature, humidity, illuminance, and divider readings work. Its standby
current is still under investigation, so the PPK2/DMM checklist below remains
mandatory for energy-harvesting deployments. See `../docs/HAT_POWER.md`.

## Automatic configurations

| ID | Detected configuration | Advertised measurements |
| ---: | --- | --- |
| 1 | Bare XIAO + USB | MCU die temperature and regulated VDD |
| 2 | Bare XIAO + external power (CJMCU expected) | MCU die temperature and regulated VDD |
| 3 | Logger HAT + USB | SHT40 ambient temperature/humidity and BH1750 lux; voltage invalid |
| 4 | Logger HAT + external power/CJMCU | SHT40 ambient temperature/humidity, BH1750 lux, and valid VSTOR when the ADC result passes validation |

At boot the firmware makes three power-gated probes of the SHT40 and BH1750
before selecting bare mode. Detection requires a verified SHT40 measurement
with valid CRCs or a non-floating BH1750 conversion, so an ACK from an unpulled
bus cannot create a phantom HAT. One healthy sensor is enough to select HAT
mode. Bare mode retries discovery every five minutes by default. Individual
measurement failures invalidate only their affected fields, while three
consecutive samples with no valid HAT sensor field demote the device to bare
mode until periodic rediscovery succeeds.

USB presence comes directly from the nRF52840
`POWER.USBREGSTATUS.VBUSDETECT` facility. Both Zephyr USB device-stack
implementations and the USBD devicetree node are disabled. A HAT voltage is
never reported as VSTOR while USB is present.

## Logger HAT V3 wiring

The mapping is fixed by the Logger HAT V3 schematic and Zephyr's `xiao_ble`
board definition:

| HAT/XIAO signal | nRF52840 mapping | Use |
| --- | --- | --- |
| D10 | P1.15 | Shared active-high SHT40/BH1750/divider enable |
| D4 / SDA | P0.04, `i2c1` | Raw I2C data |
| D5 / SCL | P0.05, `i2c1` | Raw I2C clock |
| D1 / A1 | P0.03, SAADC AIN1 | VSTOR divider input |

The HAT uses SHT40 address `0x44` and BH1750 address `0x23`; `0x5C` is not
probed. The 100 kOhm/100 kOhm divider gives `VSTOR/VADC = 2.0`. AIN1 uses the
internal 0.6 V reference, gain 1/4, 12-bit resolution, 40 us acquisition, and
64x oversampling. The internal VDD channel remains gain 1/6 for bare mode.
`NRF_SAADC_VDD` samples the rail directly, and Zephyr's ADC conversion already
reverses that gain, so its converted millivolts are advertised without further
scaling.

## Measurement and rail timing

Each HAT sample has one cleanup path that runs on success or failure:

1. Resume `i2c1`, drive D10 high, and sleep for at least 1 ms.
2. Send BH1750 Power On (`0x01`) and One-Time H-Resolution (`0x20`) first.
3. Send SHT40 high-precision (`0xFD`), sleep to its 10 ms deadline, read six
   bytes, and check both CRC-8 values (polynomial `0x31`, initial `0xFF`).
4. When USB is absent, wait until at least 25 ms after D10 rose, read AIN1, and
   suspend SAADC immediately. Accept only the configured 1.5–4.5 V plausible
   VSTOR range.
5. Sleep until 180 ms after the BH1750 measurement command, read two bytes,
   and convert `lux = raw / 1.2`.
6. Suspend `i2c1` into its high-impedance pinctrl state, then drive D10 low.

All delays use `k_sleep()` and uptime deadlines. SDA/SCL are not driven low
between samples, which avoids wasting current through the HAT's always-on
10 kOhm pull-ups.

SHT40 values use the datasheet equations:

```text
temperature_C = -45 + 175 * raw / 65535
humidity_pct  =  -6 + 125 * raw / 65535
```

Humidity is clamped to 0–100%.

## Protocol v3.1 sensor advertisements

By default, the radio behavior is unchanged: a one-second legacy,
non-connectable, non-scannable burst at 200 ms intervals and 0 dBm, followed by
System ON idle. That schedules about five advertising events; each event uses
the three primary advertising channels, so it can produce up to 15 physical
PDUs. The identity address remains fixed for the bridge MAC allowlist. The
packet ID increments once per new sample, not once per repeated event.

Every new frame includes:

- BTHome raw object `0x54`, length `03`, followed by the configured interval
  between new samples as uint24 little-endian milliseconds;
- standard BTHome device-type object `0xF0`, uint16, containing configuration
  ID 1–4.

The configured interval is not inferred by the bridge. To build a different
cadence without editing C code, for example 180 seconds:

```bash
west build -p always -b xiao_ble path/to/sensor-zephyr -- \
  -DCONFIG_SENSOR_SAMPLE_INTERVAL_MS=180000
```

The uint24 on-air value supports 1,000–16,777,215 ms. The default is 20,000 ms.

The advertising burst is independently configurable without editing C code:

```bash
west build -p always -b xiao_ble path/to/sensor-zephyr -- \
  -DCONFIG_SENSOR_ADV_BURST_MS=600
```

`CONFIG_SENSOR_ADV_BURST_MS` defaults to 1,000 ms. Shorter bursts reduce radio
energy but also reduce collision/interference redundancy; validate packet loss
from the bridge sequence gaps before changing a deployed sensor. A build
assertion reserves 250 ms of every sample interval for measurement and
scheduling, so incompatible cadence/burst combinations fail at compile time.

### Enhanced bare form

The service-data array passed to `BT_DATA_SVC_DATA16`, including UUID bytes, is
exactly 19 bytes:

```text
D2 FC 40 00 ss 02 tt tt 0C vv vv 54 03 ii ii ii F0 cc cc
```

With Flags and AD headers the complete advertisement is exactly 24 bytes.

### Logger HAT form

The service-data array is exactly 26 bytes, including the two UUID bytes:

```text
D2 FC
40
00 ss
02 tt tt
03 hh hh
05 ll ll ll
0C vv vv
54 03 ii ii ii
F0 cc cc
```

With Flags and AD headers the complete advertisement is exactly 31 bytes, at
the legacy advertising limit. Separate fixed buffers and build assertions
prevent stale HAT fields from leaking into a bare frame.

### Invalid sentinels

| Field | On-air sentinel |
| --- | --- |
| Temperature | `0x8000` (`INT16_MIN`) |
| Humidity | `0xFFFF` |
| Illuminance | `0xFFFFFF` |
| Voltage (VDD or VSTOR) | `0xFFFF` |

The firmware still broadcasts when a measurement fails. In configuration 4,
the configuration ID remains 4 even when that sample's VSTOR field is invalid.

## Build

Release:

```bash
west build -p always -b xiao_ble path/to/sensor-zephyr
```

Debug RTT logging (higher current, bench use only):

```bash
west build -p always -b xiao_ble path/to/sensor-zephyr -- \
  -DEXTRA_CONF_FILE=debug.conf
```

The release build creates `build/zephyr/zephyr.uf2`. Double-tap reset to expose
the UF2 bootloader drive, then copy that file to the drive.

## Low-power behavior

The pre-existing low-power decisions remain:

- external QSPI NOR enters Deep Power-Down at boot;
- DC/DC configuration comes from the upstream board devicetree;
- RGB LEDs are driven inactive;
- SAADC is active only around a conversion;
- the raw I2C controller uses runtime PM and its high-impedance sleep pinctrl
  state;
- D10 is low outside HAT probes and measurements;
- serial, console, logging, shell, and both USB device stacks are disabled in
  release;
- the burst and all conversion waits sleep rather than poll.

Do not assume the previous bare-board current is unchanged until the complete
cycle and sleep floor have been measured on the new firmware.

## Hardware validation checklist

Use a calibrated DMM, a Nordic PPK2, and a BLE packet capture:

1. Confirm D10 is P1.15, active high, is low during sleep, and powers the sensor
   rail only for the expected probe/measurement window.
2. Confirm D4/D5 are P0.04/P0.05 on `i2c1`; verify both pins are high impedance
   after cleanup and are never held low between samples.
3. Confirm the physical BAT+ wire is connected to BQ25570 VSTOR.
4. In bare mode, compare advertised regulated VDD with a calibrated DMM under
   both USB and external power; it should be near the actual rail, not four
   times higher.
5. Compare advertised VSTOR with a calibrated DMM at at least three points
   across the expected range and validate the 2.0 divider ratio.
6. Disconnect or suppress each sensor independently during startup. Confirm a
   verified conversion from either BH1750 or SHT40 selects HAT mode and only
   the missing sensor's fields use sentinels. With no HAT, confirm floating-bus
   responses do not select HAT mode.
7. Inject or simulate an SHT40 CRC failure and confirm only temperature and
   humidity use sentinels while the frame, lux, and eligible voltage continue.
8. Test BH1750 in darkness and under illumination; verify a failed read produces
   `0xFFFFFF`.
9. Remove or disable the detected HAT and confirm three all-invalid samples
   demote to a valid bare frame, followed by periodic rediscovery.
10. Capture exact 24-byte enhanced-bare and 31-byte HAT advertisements and
   verify the packet ID, object order, little-endian interval, and IDs 1–4.
11. Check all four power/hardware combinations. In particular, HAT + USB must
   always advertise voltage `0xFFFF`; bare + external must advertise VDD, never
   VSTOR.
12. Rebuild at multiple cadences (for example 1 s, 20 s, and 180 s) and verify
   the bridge/webapp continue without code changes.
13. Measure the PPK2 sleep baseline and full-cycle average. Confirm QSPI DPD,
    I2C/SAADC cleanup, and no unexpected USB-current plateau.
