#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fault-injection model for the sensor advertising start recovery."""

EALREADY = -114


def recover_start(start, stop) -> int:
    result = start()
    if result == EALREADY:
        stop()
        result = start()
    return result


def main() -> None:
    starts = iter((EALREADY, 0))
    calls: list[str] = []

    def injected_start() -> int:
        calls.append("start")
        return next(starts)

    def observed_stop() -> int:
        calls.append("stop")
        return 0

    assert recover_start(injected_start, observed_stop) == 0
    assert calls == ["start", "stop", "start"]

    # Non-EALREADY failures remain failures and are not hidden by an unrelated
    # stop attempt.
    calls.clear()
    assert recover_start(lambda: -5, observed_stop) == -5
    assert calls == []
    print("sensor advertising recovery model: injected EALREADY recovered")


if __name__ == "__main__":
    main()
