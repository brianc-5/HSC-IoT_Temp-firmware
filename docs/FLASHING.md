# Flash prebuilt UF2 firmware

UF2 flashing needs no compiler or debug probe. It replaces the application but
normally leaves the XIAO UF2 bootloader intact.

## Before flashing

1. Physically label one XIAO **SENSOR** and the other **BRIDGE**.
2. Disconnect any harvested/external supply from the sensor.
3. Use a known data-capable USB-C cable; charge-only cables do not expose the
   bootloader drive.
4. Download the assets and `SHA256SUMS` from the
   [latest release](https://github.com/brianc-5/HSC-IoT_Temp-firmware/releases/latest).
5. Verify the downloaded hashes using one of:

   ```sh
   shasum -a 256 -c SHA256SUMS
   # or on Linux
   sha256sum -c SHA256SUMS
   ```

## Flash the Zephyr sensor

1. Connect only the sensor XIAO by USB.
2. Double-tap its Reset button quickly. A removable UF2 bootloader drive should
   appear.
3. Copy `HSC-Tplus-sensor-zephyr.uf2` onto that drive.
4. Wait for the copy to finish. The drive normally ejects itself and the board
   restarts.
5. Do not expect a serial port from the release sensor image: USB, console,
   shell, and logging are deliberately disabled to reduce power.

The production image works on a bare XIAO or direct-stacked Logger HAT V3. It
automatically detects both hardware and USB/external power state.

## Flash the bridge

1. Connect only the bridge XIAO by USB.
2. Double-tap Reset to expose its UF2 drive.
3. Copy `HSC-Tplus-bridge-commissioning.uf2` onto the drive.
4. Reopen the USB serial port at 115200 baud.
5. Confirm the bridge prints `HSC T+ Bridge firmware v3 starting`, a warning
   that its sensor address filter is disabled, and a `[ready]` line.

The released bridge is intentionally generic so a first-time user can discover
their sensor address. It will accept any compatible HSC T+ BTHome frame in
range. Use it only in a controlled commissioning environment, then build and
flash an address-locked bridge as described in
[COMMISSIONING.md](COMMISSIONING.md).

## Optional legacy Arduino sensor

`HSC-Tplus-sensor-arduino-legacy.uf2` is the older bare-board implementation.
It reports MCU die temperature and VDD using the original 11-byte service-data
format. The current bridge remains compatible, but this image does not detect
the Logger HAT, advertise configuration IDs/cadence, or provide the Zephyr
firmware's recovery behavior. New deployments should use the Zephyr image.

## Verify the result

- The sensor should appear in nRF Connect as unencrypted BTHome Service Data
  UUID `0xFCD2`. It is non-connectable and may not have a friendly name.
- The bridge should advertise as `HSC T+ Bridge` and expose service
  `8f0e0001-9d7d-4d5a-9f2b-3c4a5b6c7d8e`.
- One `[sample]` bridge log line should appear for each new sensor sample,
  approximately every 20 seconds by default. Repeated radio packets inside the
  one-second burst are deduplicated.

## If the bootloader drive does not appear

1. Unplug all external hardware and try another data cable/USB port.
2. Double-tap Reset more quickly.
3. Ensure the board is a XIAO nRF52840, not a visually similar XIAO variant.
4. Try holding Reset while connecting USB, then release and double-tap.
5. If an SWD flash erased or replaced the bootloader, restore the correct XIAO
   nRF52840 bootloader with an external debugger before using UF2 again.

Copy errors often disappear too quickly to read because the UF2 volume ejects
on a successful reset. Re-enter the bootloader and verify the correct role by
its runtime behavior rather than relying only on the host copy dialog.

## Rollback

Reflash an earlier UF2 from its GitHub release. Bridge flash layout is designed
to remain compatible across Protocol v3 builds, but make a CSV export before a
firmware experiment whenever retained history matters. Flashing the sensor has
no history to erase.
