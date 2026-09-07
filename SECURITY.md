# Security and privacy

## Supported version

Security fixes are applied to the latest tagged release. Development snapshots
and older releases may receive fixes only when practical.

## Radio security model

HSC T+ is designed for local environmental telemetry, not confidential or
safety-critical data.

- Sensor advertisements are unencrypted BTHome v2 broadcasts. Anyone in radio
  range can observe or replay them.
- The sensor does not pair and cannot receive radio commands.
- The bridge GATT service is connectable without application-layer
  authentication or encryption requirements.
- The sensor-address allowlist reduces accidental cross-talk; it is not
  cryptographic authentication and an address can be spoofed.
- The generic commissioning bridge intentionally disables the allowlist and
  must not be left deployed in a shared radio environment.
- TIME sync and history erase are accepted from a connected GATT client.

Do not expose the bridge as a trusted actuator, access-control device, alarm,
or source of secrets. Keep it physically controlled and within the smallest
useful radio range. Add authenticated transport and threat-model it before any
higher-risk use.

## Data privacy

Firmware stores environmental measurements, signal strength, sequence data,
configuration ID, and timestamps on bridge flash. It stores no account
credentials. A stable sensor BLE address may identify a physical unit; keep
deployment-specific `bridge_config.h` and serial logs private if that matters.

The separate web app is responsible for browser storage/export and should be
reviewed under its own repository.

## Reporting a vulnerability

Open a GitHub security advisory for
`brianc-5/HSC-IoT_Temp-firmware` rather than a public issue when a report could
enable unauthorized history erasure, memory corruption, persistent denial of
service, malicious firmware distribution, or secret disclosure. Include the
affected tag/commit, reproduction steps, impact, and any suggested mitigation.

For ordinary reliability or measurement problems, use a normal issue and
follow the diagnostic checklist in `docs/TROUBLESHOOTING.md`.

## Firmware authenticity

Download UF2 files only from this repository's Releases page, verify
`SHA256SUMS`, and record the tag used. SHA-256 detects corruption or a mismatch;
it is not a substitute for a future signed/secure-boot chain. The standard XIAO
UF2 bootloader does not make this firmware a certified secure-update system.
