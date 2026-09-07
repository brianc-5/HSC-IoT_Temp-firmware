# Contributing

Contributions that improve measurement reliability, reproducibility,
low-power behavior, documentation, or tests are welcome.

1. Open an issue before a wire-protocol, flash-layout, or electrical-interface
   change so compatibility can be discussed.
2. Create a focused branch and avoid unrelated formatting changes.
3. Run every host-side test and compile affected firmware.
4. Include hardware evidence for a power or sensor-behavior claim: board/HAT
   revision, supply, firmware commit, instrument, trace, and conditions.
5. Update documentation and add regression coverage with the code change.
6. Do not commit deployment MAC addresses, tokens, private reports, generated
   build directories, or third-party files without compatible licensing.
7. Submit a pull request explaining behavior, compatibility impact, validation,
   and remaining limitations.

By submitting a contribution, you agree that it is licensed under Apache-2.0,
the project license. See `docs/DEVELOPMENT.md` for invariants and the full
release checklist.
