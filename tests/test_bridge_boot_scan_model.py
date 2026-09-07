#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Equivalence checks for the optimized bridge boot path.

Two changes to PersistentRecordStore::begin() and recordCrc() are covered:

1. recordCrc() became table driven. The table must reproduce the previous
   bit-by-bit CRC-16/MODBUS value for every input, or deployed flash stops
   validating.
2. begin() now recovers the retained ring in a single pass over the base log
   instead of two. This models both algorithms and asserts they produce the
   same ring contents, the same retained count, the same next write slot and
   the same restored sequence state over randomized flash images, including
   wrapped physical logs, torn slots and CRC failures.
"""

import random
import re
from pathlib import Path

RECORD_CAPACITY = 4096
FLASH_SLOTS = 8192


# ---------------------------------------------------------------------------
# 1. CRC-16/MODBUS: bit-by-bit (previous) versus table driven (current)
# ---------------------------------------------------------------------------

def crc_bitwise(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_table() -> list[int]:
    table = []
    for index in range(256):
        value = index
        for _ in range(8):
            value = (value >> 1) ^ 0xA001 if value & 1 else value >> 1
        table.append(value)
    return table


CRC_TABLE = build_table()


def crc_tabled(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc = (crc >> 8) ^ CRC_TABLE[(crc ^ byte) & 0xFF]
    return crc


def check_crc_equivalence() -> None:
    firmware = (
        Path(__file__).resolve().parents[1] / "bridge" / "bridge.ino"
    ).read_text(encoding="utf-8")
    table_match = re.search(
        r"CRC16_MODBUS_TABLE\[256\]\s*=\s*\{(.*?)\};",
        firmware,
        re.DOTALL,
    )
    assert table_match is not None, "firmware CRC table was not found"
    firmware_table = [
        int(value, 16)
        for value in re.findall(r"0x[0-9A-Fa-f]{4}", table_match.group(1))
    ]
    assert firmware_table == CRC_TABLE, (
        "firmware CRC table differs from the generated CRC-16/MODBUS table"
    )

    # Every single-byte input, i.e. every table entry, exercised directly.
    for byte in range(256):
        assert crc_bitwise(bytes((byte,))) == crc_tabled(bytes((byte,)))

    rng = random.Random(20240724)
    for length in (0, 1, 2, 11, 12, 16, 30, 32, 64, 255, 4096):
        for _ in range(200):
            payload = bytes(rng.randrange(256) for _ in range(length))
            assert crc_bitwise(payload) == crc_tabled(payload), payload.hex()

    # The 30-byte body of the literal firmware-v2 slot that
    # test_bridge_flash_model.py pins; its stored CRC is 0xF7E0.
    legacy_body = bytes.fromhex(
        "425243334d00000040e2010056342efbc40bbd819a"
        "000000000000000000"
    )
    assert len(legacy_body) == 30
    assert crc_bitwise(legacy_body) == crc_tabled(legacy_body) == 0xF7E0

    print("crc: table-driven CRC matches the bit-by-bit CRC on every input tested")


# ---------------------------------------------------------------------------
# 2. Boot recovery: two-pass (previous) versus single-pass (current)
# ---------------------------------------------------------------------------

def generation_newer(candidate: int, reference: int) -> bool:
    """Wrap-safe signed comparison, as in the firmware."""
    delta = (candidate - reference) & 0xFFFFFFFF
    if delta >= 0x80000000:
        delta -= 0x100000000
    return delta > 0


def first_retained_generation(generation: int) -> int:
    return generation - RECORD_CAPACITY + 1 if generation > RECORD_CAPACITY else 1


def recover_two_pass(slots):
    """The previous begin(): one pass for the newest record, one to load."""
    generation = 0
    flash_slot = 0
    have_last = False
    last_raw = 0
    extended = 0

    for index, slot in enumerate(slots):
        if slot is None:
            continue
        if not have_last or generation_newer(slot["generation"], generation):
            generation = slot["generation"]
            flash_slot = (index + 1) % FLASH_SLOTS
            have_last = True
            last_raw = slot["raw_seq"]
            extended = slot["seq"]

    records = {}
    count = 0
    if have_last:
        first = first_retained_generation(generation)
        for slot in slots:
            if slot is None:
                continue
            if slot["generation"] < first or slot["generation"] > generation:
                continue
            ring_index = slot["generation"] % RECORD_CAPACITY
            if records.get(ring_index, (None,))[0] != slot["generation"]:
                records[ring_index] = (slot["generation"], slot["payload"])
                count += 1

    return {
        "records": records,
        "count": count,
        "generation": generation,
        "flash_slot": flash_slot,
        "have_last": have_last,
        "last_raw": last_raw,
        "extended": extended,
    }


def recover_single_pass(slots):
    """The current begin(): keep the newest generation per ring index, prune."""
    generation = 0
    flash_slot = 0
    have_last = False
    last_raw = 0
    extended = 0
    records = {}

    for index, slot in enumerate(slots):
        if slot is None:
            continue
        if not have_last or generation_newer(slot["generation"], generation):
            generation = slot["generation"]
            flash_slot = (index + 1) % FLASH_SLOTS
            have_last = True
            last_raw = slot["raw_seq"]
            extended = slot["seq"]

        ring_index = slot["generation"] % RECORD_CAPACITY
        held = records.get(ring_index)
        if held is None or slot["generation"] > held[0]:
            records[ring_index] = (slot["generation"], slot["payload"])

    count = 0
    if have_last:
        first = first_retained_generation(generation)
        for ring_index in list(records):
            held_generation = records[ring_index][0]
            if held_generation < first or held_generation > generation:
                del records[ring_index]
            else:
                count += 1

    return {
        "records": records,
        "count": count,
        "generation": generation,
        "flash_slot": flash_slot,
        "have_last": have_last,
        "last_raw": last_raw,
        "extended": extended,
    }


def make_slot(generation: int) -> dict:
    return {
        "generation": generation,
        "raw_seq": generation % 256,
        "seq": generation % 65536,
        "payload": f"record-{generation}",
    }


def write_log(total_appends: int, rng: random.Random, torn_fraction: float = 0.0):
    """Emulates the append ring: sector erase at each 128-slot boundary."""
    slots = [None] * FLASH_SLOTS
    slot_index = 0
    for generation in range(1, total_appends + 1):
        if slot_index % 128 == 0:
            slots[slot_index:slot_index + 128] = [None] * 128
        if torn_fraction and rng.random() < torn_fraction:
            slots[slot_index] = None          # torn program / failed CRC
        else:
            slots[slot_index] = make_slot(generation)
        slot_index = (slot_index + 1) % FLASH_SLOTS
    return slots


def check_boot_scan_equivalence() -> None:
    rng = random.Random(918273)
    scenarios = []

    # Empty flash, partial fill, exactly full, wrapped several times.
    for appends in (0, 1, 127, 128, 1000, 4095, 4096, 4097, 8191, 8192, 8193,
                    9000, 20000, 65536):
        scenarios.append(("clean", appends, write_log(appends, rng)))

    # Torn slots and CRC failures scattered through the log.
    for appends in (4096, 9000, 20000):
        for fraction in (0.01, 0.1, 0.4):
            scenarios.append(
                (f"torn {fraction}", appends,
                 write_log(appends, rng, torn_fraction=fraction))
            )

    # Randomly corrupt already-written slots after the fact, which is what a
    # bit flip in a retained sector looks like at boot.
    for appends in (9000, 20000):
        slots = write_log(appends, rng)
        for _ in range(400):
            slots[rng.randrange(FLASH_SLOTS)] = None
        scenarios.append(("post-hoc corruption", appends, slots))

    for label, appends, slots in scenarios:
        two = recover_two_pass(slots)
        one = recover_single_pass(slots)
        assert one == two, (
            f"boot scan diverged for {label} after {appends} appends:\n"
            f"  two-pass:    count={two['count']} gen={two['generation']} "
            f"slot={two['flash_slot']}\n"
            f"  single-pass: count={one['count']} gen={one['generation']} "
            f"slot={one['flash_slot']}"
        )

    # Sanity: a long-running log really does retain a full window.
    full = recover_two_pass(write_log(20000, rng))
    assert full["count"] == RECORD_CAPACITY
    assert full["generation"] == 20000
    assert sorted(g for g, _ in full["records"].values()) == list(
        range(20000 - RECORD_CAPACITY + 1, 20001)
    )

    print(
        f"boot scan: single-pass recovery matches the two-pass recovery across "
        f"{len(scenarios)} flash images"
    )


def main() -> None:
    check_crc_equivalence()
    check_boot_scan_equivalence()
    print("bridge boot scan model: all optimization invariants passed")


if __name__ == "__main__":
    main()
