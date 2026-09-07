# Logger HAT V3 power investigation

## Current conclusion

The Logger HAT V3 works functionally with the production Zephyr firmware, but
the assembled sensor has shown substantially higher sleep current than
expected. The most likely mechanism is split-rail back-powering:

- D10 removes power from SHT40, BH1750, and the voltage divider;
- the PCF8563 RTC and the two 10 kΩ SDA/SCL pull-ups remain tied to HAT 3V3;
- when sensor VDD is off, the pull-ups can feed the unpowered sensor rail
  through signal-pin protection structures.

At a 2.7 V rail, two pull-ups held near a low clamp could contribute roughly
0.40–0.54 mA continuously. That is an estimate, not a measurement. It is large
enough to explain a result on the same scale as the HAT's active current.

The PCF8563 itself is unlikely to explain a hundreds-of-microamps plateau; NXP
specifies about 0.25 µA typical at 3.0 V and 25 °C. Board leakage, XIAO LEDs,
regulator/charger behavior, QSPI state, measurement equipment, or a damaged
part can contribute separately.

Primary references:

- [XIAO-log repository and V3 schematic](https://github.com/potblitd/XIAO-log/tree/main/hardware/V3)
- [nRF52840 GPIO electrical specification](https://docs-be.nordicsemi.com/bundle/nRF52840_PS_v1.8/page/gpio.html)
- [Sensirion SHT40 product/datasheet page](https://sensirion.com/products/catalog/SHT40)
- [NXP PCF8563 product page](https://www.nxp.com/products/analog-and-mixed-signal/real-time-clocks/real-time-clock-calendar%3APCF8563)

## What the release firmware currently does

For a direct-stacked HAT, each measurement session:

1. resumes `i2c1`;
2. drives D10 high;
3. starts one-shot SHT40 and BH1750 conversions;
4. optionally samples the gated VSTOR divider;
5. suspends SAADC and `i2c1`;
6. drives D10 low for the remaining part of the 20-second interval.

The `i2c1` sleep state is high impedance, and no MCU pull-ups are enabled. The
HAT's own pull-ups nevertheless remain powered in a direct stack. The release
image does not use D10 as the supply for the complete HAT and does not
explicitly select nRF52840 high-drive output mode.

## Confirm the cause before modifying hardware

Measure the sensor at a fixed, current-limited 2.7–3.0 V source with USB and
the debugger disconnected.

1. Record the bare-XIAO sleep floor with the release firmware.
2. Direct-stack the HAT and record the sleep floor, active peak, D10 waveform,
   and 20-second cycle average.
3. During a sleep interval, measure HAT D10, HAT 3V3, SDA, SCL, and the
   supposedly unpowered sensor rail.
4. If the sensor rail rises above ground while D10 is low, temporarily isolate
   SDA and then SCL (one at a time) and observe the current change.
5. Temporarily remove the HAT and reproduce the baseline to rule out a firmware
   state that survived reset.
6. Repeat without the harvester so converter quiescent/leakage current is not
   confused with the HAT.

Do not use a normal ammeter range whose burden voltage causes the 2.7 V rail to
sag; capture the actual rail at the board while measuring current.

## Experimental option A: power the complete HAT from D10

This was the proposed no-HAT-PCB-change experiment. It requires an interposer
or modified header connection:

```text
XIAO 3V3     X     HAT 3V3       (do not pass this pin through)

XIAO D10 / P1.15 ──┬── HAT D10
                    └── HAT 3V3

XIAO GND ────────────── HAT GND
XIAO D4/SDA ─────────── HAT D4/SDA
XIAO D5/SCL ─────────── HAT D5/SCL
XIAO D0 ─────────────── HAT D0
```

This switches the RTC, HAT pull-ups, sensors, and divider together, removing
the split-rail path. The HAT PCB itself need not be cut, but a fully direct
stack is impossible: the 3V3 header connection must be isolated.

### Conditions and limits

- Treat this as an experiment; it is **not** supported by the release UF2.
- Configure P1.15 for `H0H1` high-drive mode. The nRF52840 only guarantees the
  standard-drive high level at a 0.5 mA load, while high drive has a 5 mA
  voltage-level condition at VDD ≥ 2.7 V.
- Conservative peak load is about 1.2–1.5 mA before capacitor inrush. Measure
  the actual board rather than relying on that estimate.
- Never enable the SHT40 heater; heater current can be far beyond a safe GPIO
  supply load.
- Do not parallel GPIO pins.
- Add roughly 100 kΩ from the switched HAT rail to ground so it defaults off
  and discharges during reset.
- The HAT requires at least 2.4 V. Probe the switched rail during startup,
  I²C traffic, and both conversions. If it falls below 2.4 V, stop and use a
  load switch.
- Before power-off, stop I²C and leave SDA/SCL/D0 as inputs with no MCU pulls.
  Before using I²C after power-on, wait 2–5 ms and then initialize the bus.
- The PCF8563 loses time every cycle in this arrangement. Continue using the
  nRF52840 timer for wake scheduling and web-app/bridge time sync for records.

The current source resumes I²C before enabling D10, so a dedicated experimental
build must also reverse that startup order and explicitly select high drive.
Do not merely rewire the HAT and flash the normal release.

## Experimental option B: external PFET or load switch

A separate high-side switch avoids loading a GPIO with the complete HAT. The
switch must control the whole HAT rail, not only D10:

```text
XIAO 3V3 ── high-side switch ──┬── HAT 3V3
                               └── HAT D10
XIAO control GPIO ─────────────── switch enable
```

Isolate the HAT-side 3V3 and D10 header connections as required by the
interposer. Maintain the same I²C/D0 sequencing rules. For a PFET, check body
diode orientation, gate voltage, off-state drain leakage, and gate leakage;
low RDS(on) is not the important parameter at this load. A DMP2045U can switch
the load but its datasheet does not guarantee nanoamp off current. A dedicated
load switch with characterized shutdown leakage, such as the TPS22917 class,
is easier to budget.

## Acceptance criteria for a power fix

A proposed fix is not complete until all of these pass:

- HAT rail is at ground/off between samples with no signal-line phantom power;
- switched rail remains ≥2.4 V throughout acquisition;
- SHT40 CRCs and BH1750 readings remain valid across temperature/light range;
- VSTOR agrees with a calibrated meter at three or more voltages;
- 20-second cycle average and sleep floor are recorded with uncertainty;
- sensor still broadcasts the correct 31-byte HAT advertisement;
- three failure/demotion paths and bare-mode rediscovery still work;
- repeated cold starts and brownouts recover without latching the HAT on;
- no USB/external-source back-feed exists.

Until those measurements are published, quote the HAT path as “functionally
validated; standby-power optimization in progress.”
