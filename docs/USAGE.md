# Use the complete system

## Normal operating state

- Sensor: external/harvested power, no USB, Zephyr release image.
- Bridge: continuous USB power, address-locked Arduino image.
- Phone/tablet: Chrome on Android with Bluetooth enabled.
- Web app: <https://brianc-5.github.io/HSC-IoT_Temp/>.

The browser connects to the bridge, not the sensor. Keep the bridge within BLE
range of both sensor and phone.

## Start a session

1. Power the bridge and wait for it to advertise as `HSC T+ Bridge`.
2. Power the sensor. It normally emits its first burst after initialization.
3. Open the web app in Chrome on Android over HTTPS.
4. Tap **Connect** and choose `HSC T+ Bridge` in the browser picker.
5. Allow the app to read STATUS and synchronize the bridge clock.
6. Confirm the displayed hardware configuration and 20-second default cadence.
7. Synchronize history, then leave LIVE updates enabled.

Web Bluetooth support varies by browser/platform. The maintained target is
Chrome on Android. A native Bluetooth pairing step is not required; use the
picker shown by the web page.

## What each configuration shows

| Configuration | Temperature | Humidity | Light | Voltage | Power analysis |
| --- | --- | --- | --- | --- | --- |
| Bare + USB | MCU die | — | — | Regulated VDD | Unavailable |
| Bare + external | MCU die | — | — | Regulated VDD | Unavailable |
| HAT + USB | Ambient SHT40 | Yes | Yes | Invalid by design | Unavailable |
| HAT + external | Ambient SHT40 | Yes | Yes | VSTOR when valid | Available with validated hardware/input settings |

MCU die temperature is not the same as calibrated ambient temperature. Bare
VDD does not reveal the storage node behind a regulator, so it cannot support
the capacitor power model.

## Time and offline operation

The sensor has no wall clock. Before the first phone sync after a bridge boot,
records contain bridge uptime. The web app writes Unix epoch seconds to the
TIMESYNC characteristic. The bridge then maps eligible retained records to
wall time and appends CRC-protected time patches.

The bridge continues receiving and storing samples when the phone leaves. On
reconnection, synchronize time/history again. Records from before a power loss
remain recoverable; status flags distinguish previous-boot and epoch state.

## History retention

The bridge stores 4,096 records:

```text
retention duration = 4096 × sample interval
```

Examples:

- 20 seconds: about 22 hours 45 minutes;
- 60 seconds: about 2 days 20 hours;
- 180 seconds: about 8 days 13 hours.

When the ring is full, the oldest base record is replaced. Export CSV before
the window rolls over if long-term retention matters.

## Erase history

The web app requires a two-step confirmation. The command erases both base and
epoch-patch logs and cannot be undone. Export first. Developers can issue the
same operation by writing byte `02` to HIST_CTRL; wait for a zero-record STATUS
notification before disconnecting or removing bridge power.

## Interpret sequence gaps

The sensor increments an 8-bit packet ID once per new sample. Repeated packets
inside a burst share that ID. The bridge extends it to 16 bits and estimates
small gaps using the advertised cadence. After an outage spanning one or more
full 256-sample wraps, the exact missed count is unknowable; the bridge keeps
monotonic order and marks a resynchronization instead of inventing precision.

A few gaps can be normal in a congested 2.4 GHz environment. Persistent gaps
usually indicate range, shielding, bridge downtime, insufficient advertising
burst length, or sensor brownout.

## Power-analysis use

Only use storage-power results after:

1. HAT + external configuration is confirmed;
2. VSTOR matches a calibrated multimeter across the operating range;
3. actual storage capacitance is entered in the app;
4. dark-baseline bands are long and stable enough to agree;
5. sampling gaps and USB-powered periods are excluded.

The result is net power delivered into VSTOR. It is not raw panel-terminal
power and cannot establish converter efficiency without additional electrical
measurements.

## Routine health checks

- STATUS firmware version is 3 and record size is 16 bytes.
- Configuration ID matches the physical sensor state.
- Reported interval matches the sensor build (20,000 ms by default).
- Latest sample age resets each cadence.
- RSSI is stable enough for reliable reception.
- HAT voltage is invalid on USB and plausible on external power.
- Bridge record count grows by one per sample, not by every burst packet.
- Sensor rail/current profile remains consistent with the qualified assembly.
