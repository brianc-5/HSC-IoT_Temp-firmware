# Prebuilt firmware

Signed release pages are the canonical place for prebuilt files:

<https://github.com/brianc-5/HSC-IoT_Temp-firmware/releases/latest>

Expected assets are:

| File | Flash onto | Notes |
| --- | --- | --- |
| `HSC-Tplus-sensor-zephyr.uf2` | Sensor XIAO | Production, auto-detecting sensor |
| `HSC-Tplus-bridge-commissioning.uf2` | Bridge XIAO | Generic discovery build; address filter disabled |
| `SHA256SUMS` | Computer | Checksums for all UF2 release assets |

The legacy Arduino sensor source remains available in `sensor-arduino/`, but it
is not part of the supported release binaries.

The address-locked bridge is deployment-specific and therefore is not offered
as a universal binary. Build it after copying
`bridge/bridge_config.example.h` to the ignored `bridge/bridge_config.h` and
entering the address of your own sensor.

The workflow builds binaries from the tagged source. Binary files are not
stored in the Git history.
