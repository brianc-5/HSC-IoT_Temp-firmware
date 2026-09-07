# Build firmware from source

Released UF2 files are produced by the same commands in
`.github/workflows/firmware.yml`. The public build defaults contain no personal
sensor address.

## Supported targets and pinned versions

| Image | Framework | Board/target | Pinned dependency |
| --- | --- | --- | --- |
| Production sensor | Zephyr | `xiao_ble/nrf52840` | Zephyr v4.4.0, Zephyr SDK 1.0.1 |
| Bridge | Arduino/Bluefruit | `Seeeduino:nrf52:xiaonRF52840` | Seeed nRF52 Boards 1.1.13 |
| Legacy sensor | Arduino/Bluefruit | Same | Seeed nRF52 Boards 1.1.13 |

Use the Seeed **nRF52** package derived from the Adafruit nRF52 core. Do not use
the mbed-enabled XIAO package for either Arduino image.

## Build the bridge with Arduino CLI

Install [Arduino CLI](https://arduino.github.io/arduino-cli/latest/installation/),
then install the pinned board package:

```sh
arduino-cli config init
arduino-cli config add board_manager.additional_urls \
  https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
arduino-cli core update-index
arduino-cli core install Seeeduino:nrf52@1.1.13
```

From the repository root, a generic commissioning build is:

```sh
arduino-cli compile \
  --fqbn Seeeduino:nrf52:xiaonRF52840 \
  --output-dir build/bridge \
  bridge
```

The core produces `build/bridge/bridge.ino.hex`. Convert it to UF2 with the
`uf2conv.py` shipped in the installed Seeed core:

```sh
python3 "$HOME/Library/Arduino15/packages/Seeeduino/hardware/nrf52/1.1.13/tools/uf2conv/uf2conv.py" \
  -c -f 0xADA52840 \
  -o build/bridge/HSC-Tplus-bridge.uf2 \
  build/bridge/bridge.ino.hex
```

That path is for macOS. On Linux the package root is normally
`$HOME/.arduino15`; on Windows it is normally under
`%LOCALAPPDATA%\Arduino15`. The nRF52840 UF2 family identifier is
`0xADA52840`.

This build has no address filter. For a deployment build, first follow
[COMMISSIONING.md](COMMISSIONING.md); the compile and conversion commands are
unchanged.

You can also use Arduino IDE after adding the same board-manager URL. Select
**Seeed XIAO nRF52840** under the non-mbed **Seeed nRF52 Boards** package, open
`bridge/bridge.ino`, and choose **Sketch → Export Compiled Binary**.

## Build the legacy Arduino sensor

With the same board package:

```sh
arduino-cli compile \
  --fqbn Seeeduino:nrf52:xiaonRF52840 \
  --output-dir build/sensor-arduino \
  sensor-arduino
```

Convert `build/sensor-arduino/sensor-arduino.ino.hex` with the same `uf2conv.py`
command and family identifier. Edit the constants near the top of the sketch
to experiment with cadence, burst duration, or TX power. This is a
compatibility implementation, not the recommended HAT firmware.

## Create a Zephyr workspace

The repository is a west manifest repository pinned to Zephyr v4.4.0. The
following creates a fresh workspace on macOS or Linux; use the equivalent
Python virtual-environment activation on Windows.

```sh
mkdir hsc-tplus-workspace
cd hsc-tplus-workspace
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip west
git clone https://github.com/brianc-5/HSC-IoT_Temp-firmware.git
west init -l HSC-IoT_Temp-firmware
west update
west packages pip --install
west sdk install --version 1.0.1 --toolchains arm-zephyr-eabi
```

The SDK command may use a slightly different option spelling on another west
release; `west sdk install --help` is authoritative. Zephyr's
[v4.4 Getting Started guide](https://docs.zephyrproject.org/4.4.0/develop/getting_started/)
contains host-package prerequisites and Windows instructions.

## Build the production Zephyr sensor

From the workspace root with the virtual environment active:

```sh
west build -p always \
  -b xiao_ble/nrf52840 \
  HSC-IoT_Temp-firmware/sensor-zephyr \
  -d build/sensor-zephyr
```

Some west versions accept the shorter board name `xiao_ble`; if the qualified
name is rejected, list targets with `west boards | grep xiao_ble` and use the
name shown by the pinned Zephyr checkout.

The release UF2 is:

```text
build/sensor-zephyr/zephyr/zephyr.uf2
```

### Change the sample cadence

The default is 20,000 ms. Override Kconfig without editing C:

```sh
west build -p always \
  -b xiao_ble/nrf52840 \
  HSC-IoT_Temp-firmware/sensor-zephyr \
  -d build/sensor-180s -- \
  -DCONFIG_SENSOR_SAMPLE_INTERVAL_MS=180000
```

The on-air uint24 interval supports 1,000–16,777,215 ms. The build rejects
combinations that cannot fit measurement and advertising time inside the
chosen cadence.

### Change the advertising burst

```sh
west build -p always \
  -b xiao_ble/nrf52840 \
  HSC-IoT_Temp-firmware/sensor-zephyr \
  -d build/sensor-short-burst -- \
  -DCONFIG_SENSOR_ADV_BURST_MS=600
```

Shorter bursts save radio energy but reduce redundancy. Validate missed
sequences at the bridge before deploying a change.

### Build a debug image

```sh
west build -p always \
  -b xiao_ble/nrf52840 \
  HSC-IoT_Temp-firmware/sensor-zephyr \
  -d build/sensor-debug -- \
  -DEXTRA_CONF_FILE=debug.conf
```

Debug output uses RTT. It changes current consumption and must not be used for
sleep-current qualification.

## Host-side tests

The deterministic models require only Python 3 and do not access hardware:

```sh
python3 tests/test_bridge_flash_model.py
python3 tests/test_bridge_protocol_v3_model.py
python3 tests/test_bridge_boot_scan_model.py
python3 tests/test_sensor_adv_recovery_model.py
```

They exercise flash layout/recovery, parser behavior, cadence and sequence
handling, advertisement recovery, and equivalence of optimized boot scanning.

## Reproducible release artifacts

On every push, GitHub Actions builds the production Zephyr sensor and Arduino
bridge UF2 files and runs the host-side models. Pushing a `v*` tag creates a
GitHub Release with renamed UF2 assets and `SHA256SUMS`. The legacy Arduino
sensor remains a source-only compatibility implementation:

```sh
git tag -a v0.1.0 -m "HSC T+ Firmware v0.1.0"
git push origin v0.1.0
```

Only tag a commit whose Actions run is green. The generic bridge artifact is
for commissioning; a per-device address-locked build cannot be reproducible
from public source without intentionally disclosing the address.
