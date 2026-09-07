#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic byte/layout and recovery checks for the bridge QSPI logs."""

from dataclasses import dataclass
import struct


VISIBLE_RECORDS = 4096
PRIMARY_SLOTS = 8192
RECORDS_PER_SECTOR = 128
PATCH_SLOTS = 16384
PATCHES_PER_SECTOR = 256
FLASH_MAGIC = 0x33435242
FLASH_SLOT_SIZE = 32
FLASH_FORMAT_V3 = 3
INVALID_U16 = 0xFFFF
INTERNAL_AND_CLOCK_MASK = 0xC1

# Literal firmware-v2 slot in the exact deployed byte layout, including its
# CRC. Keeping it pinned catches accidental changes to field widths or offsets.
REAL_LEGACY_SLOT = bytes.fromhex(
    "425243334d00000040e2010056342efbc40bbd819a"
    "000000000000000000e0f7"
)


@dataclass(frozen=True)
class StoredRecord:
    time_s: int
    seq: int
    temp_centi: int
    voltage_mv: int
    lux_x1: int
    hum_centi: int
    rssi: int
    flags: int


def record_crc(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def append_crc(body: bytes) -> bytes:
    assert len(body) == 30
    return body + struct.pack("<H", record_crc(body))


def encode_legacy_slot(
    generation: int, record: StoredRecord, raw_seq: int
) -> bytes:
    core = struct.pack(
        "<IHhHbB",
        record.time_s,
        record.seq,
        record.temp_centi,
        record.voltage_mv,
        record.rssi,
        record.flags,
    )
    return append_crc(
        struct.pack("<II", FLASH_MAGIC, generation)
        + core
        + bytes((raw_seq,))
        + bytes(9)
    )


def encode_v3_slot(
    generation: int, record: StoredRecord, raw_seq: int
) -> bytes:
    core = struct.pack(
        "<IHhHbB",
        record.time_s,
        record.seq,
        record.temp_centi,
        record.voltage_mv,
        record.rssi,
        record.flags,
    )
    extension = (
        struct.pack("<HHB", record.lux_x1, record.hum_centi, FLASH_FORMAT_V3)
        + bytes(4)
    )
    return append_crc(
        struct.pack("<II", FLASH_MAGIC, generation)
        + core
        + bytes((raw_seq,))
        + extension
    )


def decode_slot(slot: bytes) -> tuple[int, StoredRecord, int] | None:
    if len(slot) != FLASH_SLOT_SIZE:
        return None
    if record_crc(slot[:30]) != struct.unpack_from("<H", slot, 30)[0]:
        return None

    magic, generation = struct.unpack_from("<II", slot)
    if magic != FLASH_MAGIC or generation == 0:
        return None
    time_s, seq, temp_centi, voltage_mv, rssi, flags = struct.unpack_from(
        "<IHhHbB", slot, 8
    )
    raw_seq = slot[20]
    extension = slot[21:30]
    marker = extension[4]
    if marker == 0:
        if extension != bytes(9):
            return None
        lux_x1 = hum_centi = INVALID_U16
        flags &= INTERNAL_AND_CLOCK_MASK
    elif marker == FLASH_FORMAT_V3:
        if extension[5:] != bytes(4):
            return None
        lux_x1, hum_centi = struct.unpack_from("<HH", extension)
        config = (flags >> 1) & 0x07
        if config > 4 or (flags & 0x10 and config != 4):
            return None
    else:
        return None

    return (
        generation,
        StoredRecord(
            time_s,
            seq,
            temp_centi,
            voltage_mv,
            lux_x1,
            hum_centi,
            rssi,
            flags,
        ),
        raw_seq,
    )


@dataclass
class Entry:
    generation: int
    valid: bool = True
    epoch: int | None = None


class FlashModel:
    def __init__(self) -> None:
        self.primary: list[Entry | None] = [None] * PRIMARY_SLOTS
        self.patches: list[Entry | None] = [None] * PATCH_SLOTS
        self.primary_slot = 0
        self.patch_slot = 0
        self.generation = 0

    @staticmethod
    def _erase_sector(items: list[Entry | None], slot: int, size: int) -> None:
        start = slot - slot % size
        items[start : start + size] = [None] * size

    def append(self) -> int:
        if self.primary_slot % RECORDS_PER_SECTOR == 0:
            self._erase_sector(self.primary, self.primary_slot, RECORDS_PER_SECTOR)
        self.generation += 1
        self.primary[self.primary_slot] = Entry(self.generation)
        self.primary_slot = (self.primary_slot + 1) % PRIMARY_SLOTS
        return self.generation

    def torn_append(self) -> None:
        if self.primary_slot % RECORDS_PER_SECTOR == 0:
            self._erase_sector(self.primary, self.primary_slot, RECORDS_PER_SECTOR)
        # The program operation never produced a CRC-valid entry.
        self.primary[self.primary_slot] = Entry(self.generation + 1, valid=False)

    def patch(self, generation: int, epoch: int, valid: bool = True) -> None:
        if self.patch_slot % PATCHES_PER_SECTOR == 0:
            self._erase_sector(self.patches, self.patch_slot, PATCHES_PER_SECTOR)
        self.patches[self.patch_slot] = Entry(generation, valid=valid, epoch=epoch)
        self.patch_slot = (self.patch_slot + 1) % PATCH_SLOTS

    def erase_all(self) -> int:
        erased_sectors = 0
        for slot in range(0, PRIMARY_SLOTS, RECORDS_PER_SECTOR):
            self._erase_sector(self.primary, slot, RECORDS_PER_SECTOR)
            erased_sectors += 1
        for slot in range(0, PATCH_SLOTS, PATCHES_PER_SECTOR):
            self._erase_sector(self.patches, slot, PATCHES_PER_SECTOR)
            erased_sectors += 1
        self.primary_slot = 0
        self.patch_slot = 0
        self.generation = 0
        return erased_sectors

    def recover(self) -> list[tuple[int, int]]:
        valid_primary = [entry for entry in self.primary if entry and entry.valid]
        if not valid_primary:
            return []
        newest = max(entry.generation for entry in valid_primary)
        first = max(1, newest - VISIBLE_RECORDS + 1)
        records = {
            entry.generation: 0
            for entry in valid_primary
            if first <= entry.generation <= newest
        }
        for patch in self.patches:
            if patch and patch.valid and patch.generation in records:
                records[patch.generation] = patch.epoch or 0
        return sorted(records.items())


def check_slot_compatibility() -> None:
    assert len(REAL_LEGACY_SLOT) == FLASH_SLOT_SIZE
    legacy = decode_slot(REAL_LEGACY_SLOT)
    assert legacy is not None
    generation, record, raw_seq = legacy
    assert generation == 77
    assert raw_seq == 0x9A
    assert record == StoredRecord(
        time_s=123456,
        seq=0x3456,
        temp_centi=-1234,
        voltage_mv=3012,
        lux_x1=INVALID_U16,
        hum_centi=INVALID_U16,
        rssi=-67,
        flags=0x81,
    )
    assert encode_legacy_slot(generation, record, raw_seq) == REAL_LEGACY_SLOT

    v3_record = StoredRecord(
        time_s=1_800_000_020,
        seq=0xABCD,
        temp_centi=2345,
        voltage_mv=3810,
        lux_x1=65434,
        hum_centi=5678,
        rssi=-42,
        flags=0xB9,  # synced + config 4 + VSTOR + ambient + epoch-valid
    )
    v3_slot = encode_v3_slot(78, v3_record, 0xCD)
    assert len(v3_slot) == FLASH_SLOT_SIZE
    assert v3_slot[20] == 0xCD
    assert struct.unpack_from("<HH", v3_slot, 21) == (65434, 5678)
    assert v3_slot[25] == FLASH_FORMAT_V3
    assert v3_slot[26:30] == bytes(4)
    assert decode_slot(v3_slot) == (78, v3_record, 0xCD)

    # Both legacy and v3 CRCs cover the extension. A torn program or one-bit
    # corruption is ignored during recovery.
    for original, changed_offset in (
        (REAL_LEGACY_SLOT, 12),
        (v3_slot, 21),
        (v3_slot, 25),
        (v3_slot, 29),
    ):
        corrupt = bytearray(original)
        corrupt[changed_offset] ^= 0x01
        assert decode_slot(bytes(corrupt)) is None

    # Even with a recomputed CRC, unsupported markers/reserved data are not
    # interpreted as a different disk generation.
    unsupported_marker = bytearray(v3_slot[:30])
    unsupported_marker[25] = 2
    assert decode_slot(append_crc(bytes(unsupported_marker))) is None

    nonzero_v3_reserved = bytearray(v3_slot[:30])
    nonzero_v3_reserved[29] = 1
    assert decode_slot(append_crc(bytes(nonzero_v3_reserved))) is None

    damaged_legacy_extension = bytearray(REAL_LEGACY_SLOT[:30])
    damaged_legacy_extension[21] = 1
    assert decode_slot(append_crc(bytes(damaged_legacy_extension))) is None


def main() -> None:
    check_slot_compatibility()

    model = FlashModel()
    for _ in range(9000):
        model.append()

    recovered = model.recover()
    assert len(recovered) == VISIBLE_RECORDS
    assert [generation for generation, _ in recovered] == list(range(4905, 9001))

    # Reach a sector boundary, then lose power between erase and program. The
    # erased sector is more than 4096 generations old, so retained data remains.
    while model.primary_slot % RECORDS_PER_SECTOR:
        model.append()
    before_torn = model.recover()
    model.torn_append()
    assert model.recover() == before_torn

    # A corrupt retained entry is skipped, never emitted as a zero/uninitialized
    # record, and does not disturb the ordering of the remaining generations.
    corrupt_generation = before_torn[-100][0]
    for entry in model.primary:
        if entry and entry.generation == corrupt_generation:
            entry.valid = False
            break
    after_corruption = model.recover()
    assert len(after_corruption) == VISIBLE_RECORDS - 1
    assert corrupt_generation not in {generation for generation, _ in after_corruption}
    assert after_corruption == sorted(after_corruption)

    # Only CRC-valid epoch patches apply. Unpatched or torn-patch records retain
    # epoch zero, which represents an unknown prior-boot timestamp.
    patchable = [generation for generation, _ in after_corruption[-4:]]
    model.patch(patchable[0], 1_800_000_000)
    model.patch(patchable[1], 1_800_000_020)
    model.patch(patchable[2], 1_800_000_040, valid=False)
    patched = dict(model.recover())
    assert patched[patchable[0]] == 1_800_000_000
    assert patched[patchable[1]] == 1_800_000_020
    assert patched[patchable[2]] == 0
    assert patched[patchable[3]] == 0

    # ERASE_ALL spans every primary and epoch-patch sector, including wrapped
    # generations. Reboot recovery cannot resurrect either kind of entry.
    assert model.erase_all() == 128
    assert model.recover() == []
    assert all(entry is None for entry in model.primary)
    assert all(entry is None for entry in model.patches)

    # A subsequent sample starts a genuinely fresh generation and survives
    # recovery normally.
    assert model.append() == 1
    assert model.recover() == [(1, 0)]

    print("bridge flash model: all recovery invariants passed")


if __name__ == "__main__":
    main()
