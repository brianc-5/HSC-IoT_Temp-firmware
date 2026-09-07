# Legacy Arduino sensor

`sensor-arduino.ino` is the earlier bare-XIAO BTHome v2 sensor firmware. It is
included so the original implementation remains buildable and understandable.

It reports:

- nRF52840 MCU die temperature;
- regulated VDD;
- one-byte packet ID.

Every 20 seconds it sends a one-second non-connectable advertisement burst at
200 ms intervals and then returns to retained System ON sleep. The current
bridge accepts this original 11-byte service-data format as configuration 0.

This image does not support Logger HAT V3, configuration IDs, advertised
cadence, humidity, illuminance, or VSTOR. Use `../sensor-zephyr/` for new
deployments.

Build with the non-mbed Seeed nRF52 Boards 1.1.13 package as described in
`../docs/BUILDING.md`. Define `DEBUG=1` only for bench diagnostics; USB serial
changes power consumption.
