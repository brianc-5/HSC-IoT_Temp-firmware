# Changelog

## 0.1.0 — initial public firmware release

- Publish the production Zephyr sensor, Arduino bridge, and legacy Arduino
  bare-sensor source under Apache-2.0 with clean public history.
- Preserve Protocol v3 / sensor advertisement revision 3.1 compatibility.
- Replace the private bridge MAC with a generic commissioning build and an
  ignored per-device configuration template.
- Document architecture, direct UF2 flashing, reproducible source builds,
  commissioning, operation, HAT V3 wiring, power investigation, harvesting
  caveats, troubleshooting, security, and development invariants.
- Pin Zephyr v4.4.0 / SDK 1.0.1 and Seeed nRF52 Boards 1.1.13 in CI.
- Add tagged-release automation for the production Zephyr sensor and Arduino
  bridge UF2 files, with SHA-256 checksums.

Known limitation: Logger HAT V3 sensing is functionally validated, but its
standby power is still under investigation. Full-HAT GPIO power is documented
as an experiment and is not enabled by the release sensor image. No stock
BQ25570/CJMCU deployment is endorsed.
