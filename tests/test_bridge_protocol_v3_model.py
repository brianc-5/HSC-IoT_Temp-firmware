#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic model checks for bridge Protocol-v3 parsing and cadence."""

from collections import deque
from dataclasses import dataclass
from statistics import median_high
import struct


BTHOME_UUID = 0xFCD2
DEVICE_INFO = 0x40
SERVICE_DATA_AD_TYPE = 0x16
INVALID_U16 = 0xFFFF
MIN_PLAUSIBLE_VSTOR_MV = 1500
MAX_PLAUSIBLE_VSTOR_MV = 4500
LEGACY_SIZE = 11
BARE_SIZE = 19
HAT_SIZE = 26
MAX_SERVICE_SIZE = HAT_SIZE
MAX_SMALL_RESYNC_STEPS = 32

OBJECT_SIZES = {
    0x00: 1,
    0x01: 1,
    0x02: 2,
    0x03: 2,
    0x04: 3,
    0x05: 3,
    0x06: 2,
    0x07: 2,
    0x08: 2,
    0x09: 1,
    0x0A: 3,
    0x0B: 3,
    0x0C: 2,
    0x0D: 2,
    0x0E: 2,
    0x0F: 1,
    0x10: 1,
    0x11: 1,
    0x12: 2,
    0x13: 2,
    0x14: 2,
    0x3A: 1,
    0x3C: 2,
    0x3D: 2,
    0x3E: 4,
    0x3F: 2,
    0x40: 2,
    0x41: 2,
    0x42: 3,
    0x43: 2,
    0x44: 2,
    0x45: 2,
    0x46: 1,
    0x47: 2,
    0x48: 2,
    0x49: 2,
    0x4A: 2,
    0x4B: 3,
    0x4C: 4,
    0x4D: 4,
    0x4E: 4,
    0x4F: 4,
    0x50: 4,
    0x51: 2,
    0x52: 2,
    0x55: 4,
    0x56: 2,
    0x57: 1,
    0x58: 1,
    0x59: 1,
    0x5A: 2,
    0x5B: 4,
    0x5C: 4,
    0x5D: 2,
    0x5E: 2,
    0x5F: 2,
    0x60: 1,
    0x61: 2,
    0x62: 4,
    0x63: 4,
    0x64: 1,
    0x65: 1,
    0xF0: 2,
    0xF1: 4,
    0xF2: 3,
}
OBJECT_SIZES.update({object_id: 1 for object_id in range(0x15, 0x30)})
TARGET_IDS = {0x00, 0x02, 0x03, 0x05, 0x0C, 0x42, 0x54, 0xF0}


@dataclass(frozen=True)
class Sample:
    seq: int
    temp_centi: int
    voltage_mv: int
    lux_x1: int
    hum_centi: int
    interval_ms: int
    configuration: int
    has_reported_interval: bool
    hat_layout: bool


def le24(value: int) -> bytes:
    return value.to_bytes(3, "little")


def object_bytes(object_id: int, value: int) -> bytes:
    size = OBJECT_SIZES[object_id]
    return bytes((object_id,)) + value.to_bytes(size, "little", signed=False)

def interval_raw_bytes(value: int, length: int = 3) -> bytes:
    if length == 0:
        payload = b""
    else:
        payload = (value & ((1 << (8 * length)) - 1)).to_bytes(
            length, "little"
        )
    return bytes((0x54, length)) + payload


def build_service(
    *,
    seq: int = 7,
    temp_centi: int = 2345,
    voltage_mv: int = 3012,
    configuration: int | None = None,
    interval_ms: int | None = None,
    hum_centi: int | None = None,
    lux_centi: int | None = None,
    objects: list[bytes] | None = None,
) -> bytes:
    if objects is None:
        objects = [
            object_bytes(0x00, seq),
            object_bytes(0x02, temp_centi & 0xFFFF),
        ]
        if hum_centi is not None:
            objects.append(object_bytes(0x03, hum_centi))
        if lux_centi is not None:
            objects.append(object_bytes(0x05, lux_centi))
        objects.append(object_bytes(0x0C, voltage_mv))
        if interval_ms is not None:
            objects.append(interval_raw_bytes(interval_ms))
        if configuration is not None:
            objects.append(object_bytes(0xF0, configuration))
    return struct.pack("<HB", BTHOME_UUID, DEVICE_INFO) + b"".join(objects)


def build_advertisement(service: bytes) -> bytes:
    flags = bytes((2, 0x01, 0x06))
    return flags + bytes((len(service) + 1, SERVICE_DATA_AD_TYPE)) + service


def parse_service(data: bytes) -> Sample | None:
    if not LEGACY_SIZE <= len(data) <= MAX_SERVICE_SIZE:
        return None
    if struct.unpack_from("<H", data)[0] != BTHOME_UUID or data[2] != DEVICE_INFO:
        return None

    values: dict[int, int] = {}
    cursor = 3
    while cursor < len(data):
        object_id = data[cursor]
        cursor += 1
        size = OBJECT_SIZES.get(object_id)
        if object_id == 0x3B:
            if cursor + 2 > len(data) or data[cursor] & 0xE0:
                return None
            size = 2 + (data[cursor] & 0x1F)
        elif object_id in (0x53, 0x54):
            if cursor >= len(data):
                return None
            size = 1 + data[cursor]
        if object_id == 0x54 and (size != 4 or data[cursor] != 3):
            return None
        if (
            size is None
            or (object_id in TARGET_IDS and object_id in values)
            or (
                object_id in (0x42, 0x54)
                and ({0x42, 0x54} & values.keys())
            )
            or cursor + size > len(data)
        ):
            return None
        if object_id in TARGET_IDS:
            value_start = cursor + 1 if object_id == 0x54 else cursor
            values[object_id] = int.from_bytes(
                data[value_start : cursor + size], "little"
            )
        cursor += size

    required = {0x00, 0x02, 0x0C}
    if not required.issubset(values):
        return None

    has_environment = bool(set(values) & {0x03, 0x05})
    has_metadata = bool(set(values) & {0x42, 0x54, 0xF0})
    if not has_environment and not has_metadata:
        configuration = 0
        interval_ms = 0
        has_interval = False
        hat_layout = False
    else:
        interval_ms = values.get(0x54, values.get(0x42, 0))
        configuration = values.get(0xF0, 0)
        if interval_ms == 0 or configuration == 0 or configuration > 4:
            return None
        has_interval = True
        bare = required | {0xF0}
        if not ({0x42, 0x54} & values.keys()):
            return None
        if not has_environment:
            if not bare.issubset(values) or configuration not in (1, 2):
                return None
            hat_layout = False
        else:
            if (
                not (bare | {0x03, 0x05}).issubset(values)
                or configuration not in (3, 4)
                or (
                    values[0x03] != INVALID_U16
                    and values[0x03] > 10_000
                )
            ):
                return None
            hat_layout = True

    voltage_mv = values[0x0C]
    hum_centi = values.get(0x03, INVALID_U16)
    illuminance_centi = values.get(0x05, 0xFFFFFF)
    if illuminance_centi == 0xFFFFFF:
        lux_x1 = INVALID_U16
    else:
        lux_x1 = min(65_534, (illuminance_centi + 50) // 100)
    if configuration == 3 or (
        configuration == 4
        and not (
            MIN_PLAUSIBLE_VSTOR_MV
            <= voltage_mv
            <= MAX_PLAUSIBLE_VSTOR_MV
        )
    ):
        voltage_mv = INVALID_U16

    return Sample(
        seq=values[0x00],
        temp_centi=struct.unpack("<h", struct.pack("<H", values[0x02]))[0],
        voltage_mv=voltage_mv,
        lux_x1=lux_x1,
        hum_centi=hum_centi,
        interval_ms=interval_ms,
        configuration=configuration,
        has_reported_interval=has_interval,
        hat_layout=hat_layout,
    )


def parse_advertisement(payload: bytes) -> Sample | None:
    cursor = 0
    while cursor < len(payload):
        field_length = payload[cursor]
        if field_length == 0:
            break
        if cursor + field_length + 1 > len(payload):
            return None
        if field_length >= 3 and payload[cursor + 1] == SERVICE_DATA_AD_TYPE:
            data = payload[cursor + 2 : cursor + field_length + 1]
            if len(data) >= 2 and struct.unpack_from("<H", data)[0] == BTHOME_UUID:
                return parse_service(data)
        cursor += field_length + 1
    return None


def wire_flags(sample: Sample, clock_synced: bool = False) -> int:
    flags = int(clock_synced)
    flags |= (sample.configuration & 0x07) << 1
    if sample.hat_layout:
        flags |= 0x20
    if (
        sample.hat_layout
        and sample.configuration == 4
        and sample.voltage_mv != INVALID_U16
    ):
        flags |= 0x10
    return flags


def encode_status(
    *,
    record_count: int,
    last_sample_age_s: int | None,
    last_rssi: int,
    clock_synced: bool,
    configuration: int,
    advertised_interval_ms: int,
) -> bytes:
    age = (
        0xFFFFFFFF
        if record_count == 0 or last_sample_age_s is None
        else last_sample_age_s
    )
    rssi = 0 if record_count == 0 else last_rssi
    return struct.pack(
        "<IIbBBBI",
        record_count,
        age,
        rssi,
        int(clock_synced),
        3,
        configuration,
        advertised_interval_ms,
    )


class SequenceTracker:
    """Model of the bridge's cadence-hint-only sequence extension."""

    def __init__(self) -> None:
        self.have_last = False
        self.last_raw = 0
        self.extended = 0
        self.last_ms: int | None = None
        self.reported_ms = 0
        self.observations: deque[int] = deque(maxlen=9)

    @property
    def learned_ms(self) -> int:
        return median_high(self.observations) if self.observations else 0

    def accept(
        self, raw: int, now_ms: int, reported_ms: int | None
    ) -> tuple[int, int, bool] | None:
        source_changed = (reported_ms is not None) != (self.reported_ms != 0)
        if source_changed:
            self.observations.clear()
        nominal_ms = reported_ms if reported_ms is not None else self.learned_ms
        elapsed_ms = (
            now_ms - self.last_ms if self.last_ms is not None else None
        )
        if (
            self.have_last
            and raw == self.last_raw
            and elapsed_ms is not None
            and nominal_ms
            and elapsed_ms < nominal_ms + nominal_ms // 2
        ):
            return None

        skipped = 0
        resync = False
        if not self.have_last:
            self.extended = raw
            self.have_last = True
        else:
            raw_delta = (raw - self.last_raw) & 0xFF
            delta = raw_delta
            if elapsed_ms is not None and nominal_ms:
                estimate = max(1, (elapsed_ms + nominal_ms // 2) // nominal_ms)
                if raw_delta == 0:
                    delta = 1 if estimate >= 256 else estimate
                    resync = True
                elif estimate >= 256:
                    resync = True
                elif (
                    raw_delta > MAX_SMALL_RESYNC_STEPS
                    and estimate <= MAX_SMALL_RESYNC_STEPS
                ):
                    delta = estimate
                    resync = True
            elif raw_delta > MAX_SMALL_RESYNC_STEPS:
                delta = 1
                resync = True
            if (
                reported_ms is None
                and not resync
                and elapsed_ms
                and 0 < raw_delta <= MAX_SMALL_RESYNC_STEPS
            ):
                observation = (elapsed_ms + raw_delta // 2) // raw_delta
                if 0 < observation <= 0xFFFFFFFF:
                    self.observations.append(observation)
            skipped = delta - 1
            self.extended = (self.extended + delta) & 0xFFFF

        self.last_raw = raw
        self.last_ms = now_ms
        self.reported_ms = reported_ms or 0
        return self.extended, skipped, resync


def check_advertisements() -> None:
    legacy = build_service(temp_centi=-321)
    bare_usb = build_service(configuration=1, interval_ms=1_000)
    bare_external = build_service(configuration=2, interval_ms=20_000)
    hat_usb = build_service(
        configuration=3,
        interval_ms=180_000,
        hum_centi=4567,
        lux_centi=12_349,
        voltage_mv=3999,
    )
    hat_external = build_service(
        configuration=4,
        interval_ms=20_000,
        hum_centi=5678,
        lux_centi=50,
        voltage_mv=3810,
    )
    assert [len(value) for value in (legacy, bare_usb, hat_usb)] == [11, 19, 26]
    assert [
        len(build_advertisement(value))
        for value in (legacy, bare_usb, hat_usb)
    ] == [16, 24, 31]

    parsed = [parse_advertisement(build_advertisement(value)) for value in (
        legacy,
        bare_usb,
        bare_external,
        hat_usb,
        hat_external,
    )]
    assert all(sample is not None for sample in parsed)
    assert [sample.configuration for sample in parsed if sample] == list(range(5))
    assert [sample.interval_ms for sample in parsed if sample] == [
        0,
        1_000,
        20_000,
        180_000,
        20_000,
    ]

    legacy_sample, _, _, hat_usb_sample, hat_external_sample = parsed
    assert legacy_sample is not None
    assert legacy_sample.lux_x1 == legacy_sample.hum_centi == INVALID_U16
    assert wire_flags(legacy_sample) == 0
    assert hat_usb_sample is not None
    assert hat_usb_sample.voltage_mv == INVALID_U16
    assert hat_usb_sample.lux_x1 == 123
    assert wire_flags(hat_usb_sample) == 0x26
    assert hat_external_sample is not None
    assert hat_external_sample.lux_x1 == 1
    assert wire_flags(hat_external_sample, clock_synced=True) == 0x39

    # Object order is deliberately not a parser dependency.
    reordered = build_service(
        objects=[
            object_bytes(0xF0, 2),
            object_bytes(0x0C, 3000),
            object_bytes(0x00, 9),
            interval_raw_bytes(20_000),
            object_bytes(0x02, (-123) & 0xFFFF),
        ]
    )
    assert len(reordered) == BARE_SIZE
    assert parse_service(reordered) == Sample(
        9, -123, 3000, INVALID_U16, INVALID_U16, 20_000, 2, True, False
    )

    # Unrelated standard objects with known fixed or length-prefixed sizes are
    # skipped without restoring any fixed-offset dependency.
    legacy_with_extras = build_service(
        objects=[
            object_bytes(0x00, 10),
            object_bytes(0x01, 97),
            object_bytes(0x46, 50),  # genuine one-byte UV-index object
            object_bytes(0x02, 2100),
            object_bytes(0x0C, 2990),
            b"\x53\x02ok",
            object_bytes(0xF1, 0x04030201),
        ]
    )
    assert len(legacy_with_extras) == 24
    extra_sample = parse_service(legacy_with_extras)
    assert extra_sample is not None
    assert (extra_sample.seq, extra_sample.configuration) == (10, 0)

    repeated_ignored = build_service(
        objects=[
            object_bytes(0x00, 11),
            object_bytes(0x01, 97),
            object_bytes(0x01, 96),
            object_bytes(0x02, 2100),
            object_bytes(0x0C, 2990),
        ]
    )
    assert parse_service(repeated_ignored) is not None

    # Sentinels survive parsing; valid illuminance rounds and saturates without
    # ever colliding with the stored uint16 invalid sentinel.
    sentinel_hat = build_service(
        configuration=4,
        interval_ms=1_000,
        hum_centi=INVALID_U16,
        lux_centi=0xFFFFFF,
        voltage_mv=INVALID_U16,
        temp_centi=-32768,
    )
    sentinel_sample = parse_service(sentinel_hat)
    assert sentinel_sample is not None
    assert (
        sentinel_sample.temp_centi,
        sentinel_sample.hum_centi,
        sentinel_sample.lux_x1,
        sentinel_sample.voltage_mv,
    ) == (-32768, INVALID_U16, INVALID_U16, INVALID_U16)
    assert wire_flags(sentinel_sample) == 0x28

    saturated = parse_service(
        build_service(
            configuration=4,
            interval_ms=1_000,
            hum_centi=0,
            lux_centi=0xFFFFFE,
        )
    )
    assert saturated is not None and saturated.lux_x1 == 65_534

    for voltage_mv in (
        MIN_PLAUSIBLE_VSTOR_MV,
        MAX_PLAUSIBLE_VSTOR_MV,
    ):
        boundary = parse_service(
            build_service(
                configuration=4,
                interval_ms=20_000,
                hum_centi=4321,
                lux_centi=12_300,
                voltage_mv=voltage_mv,
            )
        )
        assert boundary is not None
        assert boundary.voltage_mv == voltage_mv
        assert (boundary.hum_centi, boundary.lux_x1) == (4321, 123)
        assert wire_flags(boundary) == 0x38

    for voltage_mv in (
        0,
        MIN_PLAUSIBLE_VSTOR_MV - 1,
        MAX_PLAUSIBLE_VSTOR_MV + 1,
        INVALID_U16,
    ):
        out_of_range = parse_service(
            build_service(
                configuration=4,
                interval_ms=20_000,
                hum_centi=4321,
                lux_centi=12_300,
                voltage_mv=voltage_mv,
            )
        )
        assert out_of_range is not None
        assert out_of_range.voltage_mv == INVALID_U16
        # Environmental channels and source/config flags survive ADC rejection.
        assert (out_of_range.hum_centi, out_of_range.lux_x1) == (4321, 123)
        assert wire_flags(out_of_range) == 0x28


def check_rejections() -> None:
    valid_bare = build_service(configuration=1, interval_ms=20_000)
    valid_ad = build_advertisement(valid_bare)
    for end in range(len(valid_ad)):
        assert parse_advertisement(valid_ad[:end]) is None

    malformed_ad = bytearray(valid_ad)
    malformed_ad[3] += 1
    assert parse_advertisement(bytes(malformed_ad)) is None

    wrong_info = bytearray(valid_bare)
    wrong_info[2] = 0x41
    assert parse_service(bytes(wrong_info)) is None

    duplicate_required = build_service(
        objects=[
            object_bytes(0x00, 1),
            object_bytes(0x02, 2000),
            object_bytes(0x0C, 3000),
            interval_raw_bytes(20_000),
            object_bytes(0x02, 2),
        ]
    )
    assert len(duplicate_required) == BARE_SIZE
    assert parse_service(duplicate_required) is None

    for bad_length in (0, 1, 2, 4):
        malformed_raw = build_service(
            objects=[
                object_bytes(0x00, 1),
                object_bytes(0x02, 2000),
                object_bytes(0x0C, 3000),
                interval_raw_bytes(20_000, bad_length),
                object_bytes(0xF0, 1),
            ]
        )
        assert parse_service(malformed_raw) is None

    old_duration_layout = build_service(
        objects=[
            object_bytes(0x00, 1),
            object_bytes(0x02, 2000),
            object_bytes(0x0C, 3000),
            b"\x42" + le24(20_000),
            object_bytes(0xF0, 1),
        ]
    )
    old_duration = parse_service(old_duration_layout)
    assert old_duration is not None
    assert (
        old_duration.interval_ms,
        old_duration.configuration,
        old_duration.has_reported_interval,
    ) == (20_000, 1, True)

    unsupported = bytearray(valid_bare)
    unsupported[3] = 0x99
    assert parse_service(bytes(unsupported)) is None

    truncated_text = build_service(
        objects=[
            object_bytes(0x00, 1),
            object_bytes(0x02, 2000),
            object_bytes(0x0C, 3000),
            b"\x53\x05ab",
        ]
    )
    assert parse_service(truncated_text) is None

    assert parse_service(
        build_service(configuration=1, interval_ms=0)
    ) is None
    assert parse_service(
        build_service(configuration=0, interval_ms=20_000)
    ) is None
    assert parse_service(
        build_service(configuration=5, interval_ms=20_000)
    ) is None
    assert parse_service(
        build_service(configuration=3, interval_ms=20_000)
    ) is None  # HAT config in bare layout
    assert parse_service(
        build_service(
            configuration=2,
            interval_ms=20_000,
            hum_centi=5000,
            lux_centi=100,
        )
    ) is None  # bare config in HAT layout
    assert parse_service(
        build_service(
            configuration=4,
            interval_ms=20_000,
            hum_centi=10_001,
            lux_centi=100,
        )
    ) is None


def check_status_encoding() -> None:
    empty = encode_status(
        record_count=0,
        last_sample_age_s=None,
        last_rssi=-99,
        clock_synced=False,
        configuration=0,
        advertised_interval_ms=0,
    )
    assert len(empty) == 16
    assert struct.unpack("<IIbBBBI", empty) == (
        0,
        0xFFFFFFFF,
        0,
        0,
        3,
        0,
        0,
    )

    populated = encode_status(
        record_count=123,
        last_sample_age_s=7,
        last_rssi=-61,
        clock_synced=True,
        configuration=4,
        advertised_interval_ms=180_000,
    )
    assert struct.unpack("<IIbBBBI", populated) == (
        123,
        7,
        -61,
        1,
        3,
        4,
        180_000,
    )


def check_sequence_and_cadence() -> None:
    # Burst repeats inside 1.5 cadence periods are duplicates.
    tracker = SequenceTracker()
    assert tracker.accept(10, 0, 20_000) == (10, 0, False)
    for repeated_at in (1, 199, 19_999, 29_999):
        assert tracker.accept(10, repeated_at, 20_000) is None
    assert tracker.accept(11, 30_000, 20_000) == (11, 0, False)

    # The same raw ID after the duplicate window is accepted through the
    # restart/resync path instead of hiding a rebooted sensor indefinitely.
    rebooted = SequenceTracker()
    assert rebooted.accept(10, 0, 20_000) == (10, 0, False)
    assert rebooted.accept(10, 40_000, 20_000) == (12, 1, True)

    # Reported 1 s, 20 s, and 180 s intervals all drive the same resync logic.
    for cadence in (1_000, 20_000, 180_000):
        timed = SequenceTracker()
        assert timed.accept(1, 0, cadence) == (1, 0, False)
        assert timed.accept(200, cadence * 2, cadence) == (3, 1, True)

    # A live cadence change is consumed from the new packet; no bridge restart
    # or code change is needed.
    changing = SequenceTracker()
    assert changing.accept(40, 0, 20_000) == (40, 0, False)
    assert changing.accept(41, 180_000, 180_000) == (41, 0, False)
    assert changing.accept(220, 540_000, 180_000) == (43, 1, True)
    assert changing.reported_ms == 180_000

    # An irregular legacy stream learns a rolling robust median. The learned
    # cadence is an inference hint only and never rejects a changed packet ID.
    legacy = SequenceTracker()
    for raw, timestamp in enumerate((0, 19_000, 41_000, 60_000, 81_000), 1):
        result = legacy.accept(raw, timestamp, None)
        assert result is not None
    assert legacy.learned_ms == 21_000
    assert legacy.accept(200, 123_000, None) == (7, 1, True)

    # With no cadence evidence, a large jump advances exactly once and is
    # explicitly a resync instead of assuming a 20-second interval.
    unknown = SequenceTracker()
    assert unknown.accept(5, 0, None) == (5, 0, False)
    assert unknown.accept(240, 100, None) == (6, 0, True)

    # Normal uint8 wrap and long plausible outages retain monotonic extension.
    wrapping = SequenceTracker()
    assert wrapping.accept(255, 0, 1_000) == (255, 0, False)
    assert wrapping.accept(0, 1_000, 1_000) == (256, 0, False)
    assert wrapping.accept(100, 101_000, 1_000) == (356, 99, False)

    full_wrap = SequenceTracker()
    assert full_wrap.accept(1, 0, 1_000) == (1, 0, False)
    # The modulo advance is one, but elapsed time spans a complete wrap, so the
    # count is intentionally marked as a resync rather than claimed exact.
    assert full_wrap.accept(2, 256_000, 1_000) == (2, 0, True)


def main() -> None:
    check_advertisements()
    check_rejections()
    check_status_encoding()
    check_sequence_and_cadence()
    print("bridge protocol v3 model: all parser/cadence invariants passed")


if __name__ == "__main__":
    main()
