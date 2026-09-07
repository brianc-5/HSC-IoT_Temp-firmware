# Development guide

## Start here

Read [ARCHITECTURE.md](ARCHITECTURE.md) and the frozen
[PROTOCOL.md](PROTOCOL.md), then build all three images with
[BUILDING.md](BUILDING.md). Keep experimental hardware changes behind explicit
configuration and do not change defaults until they have target measurements.

## Compatibility rules

Changes to any of these require coordinated bridge, sensor, tests, protocol
documentation, and separate web-app work:

- BTHome object order/encoding or advertisement lengths;
- configuration IDs or invalid sentinels;
- Record/STATUS byte layouts or GATT UUIDs;
- history opcodes and stream markers;
- flash slot offsets, commit markers, CRC, or retained capacity;
- cadence/sequence deduplication semantics.

Protocol v3 retains the original legacy sensor parser and the previous
32-byte base flash-slot offsets. Preserve that compatibility unless a new
major protocol explicitly provides migration behavior.

## Sensor development

The production sensor intentionally avoids unnecessary runtime facilities:
USB device stacks, console, shell, logging, and heap are off in release. QSPI
enters deep power-down, SAADC is suspended outside conversions, and `i2c1`
uses runtime PM. Long waits use sleeping APIs.

When adding a sensor field:

1. Confirm a BTHome object and exact wire width.
2. Recalculate the full legacy advertisement; HAT mode already uses all 31
   bytes.
3. Define an invalid sentinel and failure-local behavior.
4. Add compile-time size assertions.
5. Extend bridge parsing defensively.
6. Extend Record/STATUS only through a versioned protocol change.
7. Add model cases for malformed, missing, reordered, and saturated values.
8. Measure sleep floor and cycle energy on target hardware.

## Bridge development

The Bluefruit scan callback is latency-sensitive. Parse/copy the report and
resume scanning before flash work, logging, or notification work. BLE callbacks
communicate with the main loop through bounded synchronization primitives.

Persistent-store invariants:

- a slot is trusted only when its marker and CRC are valid;
- writes become committed only after their payload is durable;
- recovery is deterministic with erased, torn, corrupt, and wrapped regions;
- history iteration never exposes partially written state;
- erasure reports completion and does not silently retain epoch patches.

The generic bridge must stay visibly marked as filter-disabled. Do not add a
real sensor address to tracked source, tests, examples, binaries, or logs.

## Tests

Run before every commit:

```sh
python3 tests/test_bridge_flash_model.py
python3 tests/test_bridge_protocol_v3_model.py
python3 tests/test_bridge_boot_scan_model.py
python3 tests/test_sensor_adv_recovery_model.py
```

Then compile all firmware. Hardware release checks should cover:

- exact 24-byte enhanced-bare and 31-byte HAT advertisements;
- all four configuration IDs;
- 20-second cadence and at least one alternate cadence;
- address accept/reject behavior;
- bridge scan continuity during full history transfer;
- power-cycle recovery with torn/corrupt flash fixtures;
- sensor HAT demotion and rediscovery;
- USB/external transitions without unsafe simultaneous supplies;
- PPK2 sleep/current trace for any power-affecting change.

## Style and review

- Prefer fixed-width integer types at wire/storage boundaries.
- Encode/decode little-endian fields explicitly; never cast packed buffers to
  structs.
- Validate every length before access and reject unknown variable-size fields.
- Keep ISR/callback work bounded and non-blocking.
- Use one cleanup path for peripheral power state.
- Document units in names and comments.
- Treat external measurements as evidence with setup/uncertainty, not universal
  specifications.

## Release checklist

1. Confirm no personal MAC, token, path, or private-source document is tracked.
2. Run host tests and both toolchain builds in CI.
3. Review Protocol and web-app compatibility.
4. Flash tagged Zephyr sensor and address-locked bridge on hardware.
5. Exercise HAT and bare configurations, history, time sync, and erase.
6. Record power qualification and unresolved limitations in release notes.
7. Tag `vMAJOR.MINOR.PATCH`; let Actions create UF2 files and SHA256SUMS.
8. Download the release assets, verify hashes, and perform a clean UF2 flash.
