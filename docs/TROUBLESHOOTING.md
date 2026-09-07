# Troubleshooting

## Bridge does not appear in the browser picker

- Confirm the bridge—not the sensor—has the Arduino bridge image.
- Keep the bridge on a data-capable USB connection and check the 115200-baud
  log for `[ready]`.
- Close nRF Connect or another app that may already hold the connection.
- Enable Bluetooth and browser site permission.
- Use Chrome on Android and the HTTPS-hosted app.
- Reset the bridge; connectable advertising restarts automatically after a
  disconnect.

## Sensor cannot be connected in nRF Connect

This is expected. The sensor is a non-connectable broadcaster. Inspect its
advertising Service Data UUID `0xFCD2`; connect to `HSC T+ Bridge` for GATT.

## Bridge is ready but no samples arrive

1. Wait at least 20 seconds.
2. Move sensor and bridge within a metre for commissioning.
3. Flash the generic bridge and inspect its log. If it sees a different sensor
   address, rebuild the allowlist with the address actually printed.
4. Check the sensor with an advertisement scanner.
5. Disconnect external power and test the sensor on USB to rule out harvester
   brownout.
6. Verify the bridge uses the non-mbed Seeed nRF52 board core.

An address-locked bridge logs otherwise valid senders as `[scan] rejected
BTHome advertiser=...`.

## Wrong configuration ID

- HAT detection requires a verified SHT40 CRC-protected reading or a valid
  BH1750 conversion. Check D10, D4/SDA, D5/SCL, GND, orientation, and the
  `0x44`/`0x23` addresses.
- The sensor probes three times at boot. Bare mode retries HAT discovery every
  five minutes by default.
- Three consecutive samples with no valid HAT field demote to bare mode.
- USB/external classification uses the nRF52840 VBUS detector. It does not
  indicate which external converter is fitted.

## Temperature works but humidity/light/voltage is invalid

- SHT40 CRC failure invalidates temperature and humidity together.
- BH1750 failure uses illuminance sentinel `0xFFFFFF`.
- HAT + USB deliberately invalidates voltage.
- HAT + external accepts VSTOR only within the configured 1.5–4.5 V band.
- Check the HAT BAT+ to VSTOR wire and validate the D1/A1 2:1 divider.

The sensor still broadcasts a complete frame when one channel fails.

## Reported HAT voltage is about half or four times expected

- Confirm this is HAT VSTOR, not bare regulated VDD.
- The V3 divider is on D1/A1 and has a nominal ratio of 2.0.
- Ensure the firmware uses SAADC AIN1, gain 1/4 for VSTOR, and does not apply a
  second gain correction after Zephyr's millivolt conversion.
- Measure both sides of the divider with a high-impedance meter at several
  voltages before changing software calibration.

## Current consumption is too high

1. Remove USB, debugger, LEDs, and unrelated accessories from the measurement.
2. Compare bare XIAO, direct-stacked HAT, and harvester separately.
3. Confirm QSPI deep power-down, SAADC suspension, D10 low time, and `i2c1`
   runtime suspension.
4. Probe the HAT sensor rail while D10 is low; a raised rail suggests
   signal-line back-powering from the always-on pull-ups.
5. Read [HAT_POWER.md](HAT_POWER.md) before attempting full-HAT GPIO power.

Debug firmware, serial consoles, and USB change the result. Quote sleep floor,
active peak, and full-cycle average separately.

## UF2 copy fails or board reboots repeatedly

- Confirm the UF2 matches XIAO nRF52840.
- Use another cable/port and remove external circuitry.
- Re-enter bootloader with a quick Reset double-tap.
- A sensor that runs on USB but resets on harvested power is usually seeing
  rail collapse during startup, measurement, or radio peak.
- If the bootloader drive never appears, it may need SWD recovery.

## History does not have wall-clock timestamps

The bridge records before time sync using uptime. Connect the web app and let
it write TIMESYNC before requesting history. Keep the transfer connected until
the `FF` terminator. Previous-boot records that cannot be mapped safely remain
explicit rather than receiving guessed epochs.

## History transfer stops

- Enable HIST_DATA notifications before writing HIST_CTRL.
- Request all history with `01 00 00 00 00`.
- Keep the page foregrounded and the phone near the bridge.
- Reconnect and request again; streaming does not erase records.
- `FF` is the successful completion marker. Error markers are documented in
  [PROTOCOL.md](PROTOCOL.md).

## Web app shows no power analysis

That is expected unless the status is HAT + external and VSTOR is valid. Bare
VDD and USB-powered HAT data cannot be used. Enter the actual capacitor value,
collect a stable dark baseline, and confirm the divider before interpreting
the result.

## Collect a useful bug report

Include:

- repository tag/commit and which UF2/source build was used;
- sensor and bridge board revisions;
- HAT revision and direct-stack/interposer wiring;
- full bridge startup plus one sample log (redact the address if desired);
- STATUS bytes and one raw advertisement;
- supply voltage at the board during failure;
- current-instrument model/setup for power issues;
- exact reproduction steps and expected/observed result.
