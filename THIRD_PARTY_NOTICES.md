# Third-party references and dependencies

This repository contains original HSC T+ source and documentation. It links to
but does not vendor the following hardware source material:

- [potblitd/XIAO-log](https://github.com/potblitd/XIAO-log), including the
  Logger HAT V3 schematic, is distributed by its authors under the MIT License.
  Hardware names, pin mappings, device addresses, and component values are
  referenced for interoperability and attribution.

Build/runtime dependencies are fetched separately and retain their own
licenses:

- [Zephyr Project](https://github.com/zephyrproject-rtos/zephyr), Apache-2.0
  with separately licensed modules declared by Zephyr;
- [Seeed Studio Adafruit nRF52 Arduino core](https://github.com/Seeed-Studio/Adafruit_nRF52_Arduino),
  which includes Bluefruit, TinyUSB, FreeRTOS, nrfx, and other components under
  their respective licenses;
- [BTHome v2](https://bthome.io/format/), used as the public sensor
  advertisement format.

No source PDFs, vendor datasheets, web-app libraries, or upstream XIAO-log
images/PCB files are copied into this repository. Follow links to obtain the
current authoritative versions and their license terms.
