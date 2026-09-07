# Commission a sensor and bridge

Commissioning binds a continuously powered bridge to one sensor's stable BLE
identity. It is a simple allowlist, not cryptographic pairing.

## Why there are two bridge builds

- **Generic commissioning build:** filter disabled, accepts every compatible
  HSC T+ frame in range, and prints the sender address. This is the released
  universal UF2.
- **Address-locked deployment build:** accepts only the six configured address
  bytes. It must be built for your hardware.

The bridge GATT service itself remains unauthenticated in both builds. Read
[SECURITY.md](../SECURITY.md) before deploying in a shared or hostile radio
environment.

## Discover the sensor address

1. Flash the production sensor and generic bridge UF2 files.
2. Keep other HSC T+ sensors powered off or out of range.
3. Open the bridge USB serial port at 115200 baud.
4. Wait at least one sensor cadence—20 seconds by default.
5. Find a line like:

   ```text
   [sample] sensor=AA:BB:CC:DD:EE:FF raw_seq=... config=... interval=20000 ms ...
   ```

6. Record the address exactly as printed. The Zephyr controller derives a
   stable static-random identity for the board, so reflashing the application
   does not normally change it.

If more than one address is accepted, turn off all but the intended sensor,
erase any unwanted bridge history, and repeat.

## Create the local configuration

From the repository root:

```sh
cp bridge/bridge_config.example.h bridge/bridge_config.h
```

Edit the ignored `bridge/bridge_config.h`:

```cpp
#pragma once

#define HSC_SENSOR_ADDRESS_FILTER_ENABLED true
#define HSC_SENSOR_ADDRESS_BYTES 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
```

Use the same left-to-right order printed in the serial log. Do not reverse the
bytes. The local file is listed in `.gitignore` so a deployment address is not
published accidentally.

## Build, flash, and verify

Build the bridge using [BUILDING.md](BUILDING.md), flash the resulting UF2, and
reopen the serial monitor. Startup must show:

```text
[commissioning] sensor allowlist=AA:BB:CC:DD:EE:FF
```

Then verify:

1. The intended sensor produces one `[sample]` line per new sample.
2. A different compatible sensor produces a `[scan] rejected BTHome
   advertiser=...` line and no stored record.
3. The web app connects to `HSC T+ Bridge` and receives LIVE data.
4. STATUS reports configuration ID 1–4 and the expected cadence.

## Clear commissioning/test history

Use the web app's two-step erase function, or write opcode `02` to HIST_CTRL
with a BLE development tool. The bridge erases both QSPI logs and confirms a
zero-record STATUS. Its current time synchronization remains active until
reboot.

Erasure is irreversible. Export CSV first if the data matters.

## Replace a sensor

1. Flash the generic bridge UF2 again.
2. Power only the replacement sensor and capture its new address.
3. Update the local `bridge_config.h`.
4. Rebuild/reflash the address-locked bridge.
5. Clear history if records from the two physical sensors must not be mixed.

Do not add multiple GPIO pins or infer identity from signal strength. For a
future multi-sensor design, extend the protocol and record schema explicitly
rather than disabling filtering permanently.
