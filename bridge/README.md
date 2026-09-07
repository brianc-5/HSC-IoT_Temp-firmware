# HSC T+ bridge firmware

This Arduino/Bluefruit sketch targets a USB-powered Seeed Studio XIAO
nRF52840 using Seeed nRF52 Boards 1.1.13 (non-mbed core).

It simultaneously scans non-connectable sensor advertisements and exposes a
connectable GATT service to the web app. It deduplicates burst repeats, stores
4,096 CRC-protected records in QSPI flash, patches wall-clock epochs after
TIMESYNC, and streams history at the negotiated ATT MTU.

## Configuration

With no local header, the sketch builds in generic commissioning mode and
accepts every compatible HSC T+ sensor. For normal deployment:

```sh
cp bridge/bridge_config.example.h bridge/bridge_config.h
```

Enter the address printed by the generic bridge and keep
`HSC_SENSOR_ADDRESS_FILTER_ENABLED` true. The local file is ignored by Git.

See `../docs/COMMISSIONING.md`, `../docs/BUILDING.md`, and
`../docs/PROTOCOL.md`.

## Operational requirements

- Keep the bridge continuously USB-powered.
- Use 115200-baud serial output for commissioning/diagnostics.
- Do not put flash operations or long waits in BLE callbacks.
- Preserve the 16-byte record/status contracts and flash offsets unless a
  versioned migration is implemented.
- Use the web app or HIST_CTRL guarded erase command to clear history.
