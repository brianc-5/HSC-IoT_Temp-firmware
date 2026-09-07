/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * HSC T+ Bridge firmware for the Seeed Studio XIAO nRF52840.
 *
 * Board package: "Seeed nRF52 Boards" (Adafruit nRF52 core / S140), not
 * the mbed-enabled package.  The bridge is USB powered, so the radio stays in
 * simultaneous peripheral + observer operation continuously.
 *
 * Storage is deliberately hidden behind PersistentRecordStore's append(),
 * iterateSince(), count(), and clear() interface.  A v2 QSPI/LittleFS backend
 * can replace that class without changing the BLE service code.
 */

#include <Arduino.h>
#include <bluefruit.h>
#include <limits.h>
#include <stddef.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_FlashTransport.h>

// A local bridge_config.h is optional. The public/default build accepts every
// compatible HSC T+ sensor so a new installation can discover its address.
// Copy bridge_config.example.h to bridge_config.h and enable the allowlist for
// normal operation. bridge_config.h is ignored by Git to avoid publishing a
// deployment-specific address accidentally.
#ifdef __has_include
#if __has_include("bridge_config.h")
#include "bridge_config.h"
#endif
#endif

#ifndef HSC_SENSOR_ADDRESS_FILTER_ENABLED
#define HSC_SENSOR_ADDRESS_FILTER_ENABLED false
#endif

#ifndef HSC_SENSOR_ADDRESS_BYTES
#define HSC_SENSOR_ADDRESS_BYTES 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

// Keep Arduino's generated function prototypes valid for helpers that use
// these types before the sketch preprocessor sees their full definitions.
struct StoredRecord;
struct TimeSnapshot;

// ---------------------------------------------------------------------------
// Immutable protocol constants
// ---------------------------------------------------------------------------

static const char SERVICE_UUID[] = "8f0e0001-9d7d-4d5a-9f2b-3c4a5b6c7d8e";
static const char LIVE_UUID[] = "8f0e0002-9d7d-4d5a-9f2b-3c4a5b6c7d8e";
static const char HIST_CTRL_UUID[] = "8f0e0003-9d7d-4d5a-9f2b-3c4a5b6c7d8e";
static const char HIST_DATA_UUID[] = "8f0e0004-9d7d-4d5a-9f2b-3c4a5b6c7d8e";
static const char TIMESYNC_UUID[] = "8f0e0005-9d7d-4d5a-9f2b-3c4a5b6c7d8e";
static const char STATUS_UUID[] = "8f0e0006-9d7d-4d5a-9f2b-3c4a5b6c7d8e";

// Shown in the phone's Bluetooth picker. Connection filtering uses the
// service UUID, never this string, so renaming it is safe - but the webapp
// and this firmware should be flashed/deployed together for a matching name.
static const char DEVICE_NAME[] = "HSC T+ Bridge";

// Commissioned sensor address in normal display order. The generic build is
// intentionally discovery-only. A fixed allowlist is safer for deployment
// than trusting whichever compatible public BTHome advertiser is nearby.
static const bool SENSOR_ADDRESS_FILTER_ENABLED =
    HSC_SENSOR_ADDRESS_FILTER_ENABLED;
static const uint8_t SENSOR_ADDRESS[6] = {HSC_SENSOR_ADDRESS_BYTES};

static const uint16_t BTHOME_UUID = 0xFCD2;
static const uint8_t BTHOME_DEVICE_INFO = 0x40;
static const uint8_t BTHOME_PACKET_ID = 0x00;
static const uint8_t BTHOME_TEMPERATURE_ID = 0x02;
static const uint8_t BTHOME_HUMIDITY_ID = 0x03;
static const uint8_t BTHOME_ILLUMINANCE_ID = 0x05;
static const uint8_t BTHOME_VOLTAGE_ID = 0x0C;
static const uint8_t BTHOME_DURATION_ID = 0x42;
static const uint8_t BTHOME_RAW_ID = 0x54;
static const uint8_t BTHOME_DEVICE_TYPE_ID = 0xF0;

static const size_t LEGACY_SERVICE_DATA_SIZE = 11;
static const size_t BARE_SERVICE_DATA_SIZE = 19;
static const size_t HAT_SERVICE_DATA_SIZE = 26;
static const size_t MAX_SERVICE_DATA_SIZE = HAT_SERVICE_DATA_SIZE;
static const size_t RECORD_SIZE = 16;
static const size_t STATUS_SIZE = 16;
static const size_t RECORD_CAPACITY = 4096;
static const uint8_t FW_VERSION = 3;
static const uint16_t INVALID_U16 = 0xFFFF;
// Keep this canonical inclusive band equal to sensor Kconfig and PROTOCOL.md.
static const uint16_t MIN_PLAUSIBLE_VSTOR_MV = 1500;
static const uint16_t MAX_PLAUSIBLE_VSTOR_MV = 4500;
static const uint8_t RECORD_FLAG_CLOCK_SYNCED = 0x01;
static const uint8_t RECORD_FLAG_CONFIG_SHIFT = 1;
static const uint8_t RECORD_FLAG_CONFIG_MASK = 0x0E;
static const uint8_t RECORD_FLAG_VSTOR_VALID = 0x10;
static const uint8_t RECORD_FLAG_TEMP_AMBIENT = 0x20;
static const uint8_t RECORD_FLAG_PREVIOUS_BOOT = 0x40;
static const uint8_t RECORD_FLAG_EPOCH_VALID = 0x80;
static const uint8_t RECORD_WIRE_FLAG_MASK = 0x3F;
static const uint8_t STATUS_FLAG_CLOCK_SYNCED = 0x01;

// The peripheral advertises an ATT MTU ceiling of 247. History notifications
// size themselves to the negotiated MTU: one count byte plus complete records.
static const uint16_t MAX_ATT_MTU = 247;
static const size_t MAX_RECORDS_PER_NOTIFICATION =
    (MAX_ATT_MTU - 3 - 1) / RECORD_SIZE;
static const size_t MAX_HISTORY_NOTIFICATION_SIZE =
    1 + MAX_RECORDS_PER_NOTIFICATION * RECORD_SIZE;

static const uint16_t SCAN_INTERVAL_UNITS = 160;  // 100 ms at 0.625 ms/unit.
static const uint16_t SCAN_WINDOW_UNITS = 160;    // 100% scan duty cycle.
static const uint16_t ADV_FAST_INTERVAL_UNITS = 320;  // 200 ms.
static const uint16_t ADV_SLOW_INTERVAL_UNITS = 640;  // 400 ms.
static const uint8_t LEGACY_CADENCE_WINDOW = 9;
static const uint8_t MAX_SMALL_RESYNC_STEPS = 32;
static const uint8_t HISTORY_REQUEST_OPCODE = 0x01;
static const uint8_t HISTORY_ERASE_ALL_OPCODE = 0x02;

// ---------------------------------------------------------------------------
// Little-endian helpers (never rely on host alignment or struct layout)
// ---------------------------------------------------------------------------

static uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         (static_cast<uint16_t>(data[1]) << 8);
}

static uint32_t readLe24(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16);
}

static uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

static void writeLe16(uint8_t* data, uint16_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
}

static void writeLe32(uint8_t* data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

// ---------------------------------------------------------------------------
// Wrap-safe uptime and epoch model
// ---------------------------------------------------------------------------

struct TimeSnapshot {
  uint32_t uptime_s;
  uint64_t uptime_ms;
  int64_t epoch_offset;
  bool synced;
};

// Explicit prototypes keep Arduino's sketch preprocessor from emitting these
// before the custom types they reference.
static uint32_t epochForUptime(uint32_t uptime_s,
                               const TimeSnapshot& clock);

class BridgeClock {
 public:
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == NULL) {
      return false;
    }

    // Include setup time in "seconds since boot" instead of resetting uptime.
    last_ticks_ = xTaskGetTickCount();
    uptime_ticks_ = last_ticks_;
    epoch_offset_ = 0;
    synced_ = false;
    return true;
  }

  TimeSnapshot snapshot() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    updateLocked();

    TimeSnapshot result;
    result.uptime_s = uptimeSecondsLocked();
    result.uptime_ms = uptimeMillisecondsLocked();
    result.epoch_offset = epoch_offset_;
    result.synced = synced_;

    xSemaphoreGive(mutex_);
    return result;
  }

  TimeSnapshot sync(uint32_t epoch) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    updateLocked();

    const uint32_t uptime_s = uptimeSecondsLocked();
    epoch_offset_ = static_cast<int64_t>(epoch) -
                    static_cast<int64_t>(uptime_s);
    synced_ = true;

    TimeSnapshot result;
    result.uptime_s = uptime_s;
    result.uptime_ms = uptimeMillisecondsLocked();
    result.epoch_offset = epoch_offset_;
    result.synced = true;

    xSemaphoreGive(mutex_);
    return result;
  }

 private:
  void updateLocked() {
    const TickType_t now = xTaskGetTickCount();

    // This Seeed core derives millis() from a 1024 Hz FreeRTOS tick.  Extending
    // the raw unsigned tick count makes its ~48-day wrap exact (and avoids the
    // scaled millis() discontinuity). loop() refreshes this at least once per
    // second, removing the ambiguity of more than one complete wrap.
    uptime_ticks_ += static_cast<TickType_t>(now - last_ticks_);
    last_ticks_ = now;
  }

  uint32_t uptimeSecondsLocked() const {
    const uint64_t seconds = uptime_ticks_ / configTICK_RATE_HZ;
    return seconds > UINT32_MAX ? UINT32_MAX
                                : static_cast<uint32_t>(seconds);
  }

  uint64_t uptimeMillisecondsLocked() const {
    const uint64_t seconds = uptime_ticks_ / configTICK_RATE_HZ;
    const uint64_t remainder = uptime_ticks_ % configTICK_RATE_HZ;
    return seconds * 1000ULL +
           remainder * 1000ULL / configTICK_RATE_HZ;
  }

  SemaphoreHandle_t mutex_ = NULL;
  TickType_t last_ticks_ = 0;
  uint64_t uptime_ticks_ = 0;
  int64_t epoch_offset_ = 0;
  bool synced_ = false;
};

static uint32_t epochForUptime(uint32_t uptime_s,
                               const TimeSnapshot& clock) {
  if (!clock.synced) {
    return 0;
  }

  const int64_t epoch = clock.epoch_offset +
                        static_cast<int64_t>(uptime_s);
  if (epoch <= 0) {
    return 0;
  }
  if (epoch >= static_cast<int64_t>(UINT32_MAX)) {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(epoch);
}

// ---------------------------------------------------------------------------
// Storage abstraction
// ---------------------------------------------------------------------------

// time_s is current-boot uptime until an epoch is known, then it is normalized
// in place to Unix epoch seconds. Internal flag bits never leave the bridge.
struct __attribute__((packed)) StoredRecord {
  uint32_t time_s;
  uint16_t seq;
  int16_t temp_centi;
  uint16_t voltage_mv;
  uint16_t lux_x1;
  uint16_t hum_centi;
  int8_t rssi;
  uint8_t flags;
};

static_assert(sizeof(StoredRecord) == RECORD_SIZE,
              "StoredRecord must remain exactly 16 bytes");

// Table-driven CRC-16/MODBUS (reflected polynomial 0xA001). Byte for byte this
// produces exactly the value the previous bit-by-bit loop produced - deployed
// flash stays readable - but it retires eight shift/xor steps per byte for one
// table lookup. That matters at boot, where every one of the 8,192 base slots
// and 16,384 patch slots is CRC-checked before the bridge starts scanning.
// 512 bytes of .rodata on a 1 MB part.
static const uint16_t CRC16_MODBUS_TABLE[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241,
    0xC601, 0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440,
    0xCC01, 0x0CC0, 0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40,
    0x0A00, 0xCAC1, 0xCB81, 0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841,
    0xD801, 0x18C0, 0x1980, 0xD941, 0x1B00, 0xDBC1, 0xDA81, 0x1A40,
    0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01, 0x1DC0, 0x1C80, 0xDC41,
    0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0, 0x1680, 0xD641,
    0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081, 0x1040,
    0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441,
    0x3C00, 0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41,
    0xFA01, 0x3AC0, 0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840,
    0x2800, 0xE8C1, 0xE981, 0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41,
    0xEE01, 0x2EC0, 0x2F80, 0xEF41, 0x2D00, 0xEDC1, 0xEC81, 0x2C40,
    0xE401, 0x24C0, 0x2580, 0xE541, 0x2700, 0xE7C1, 0xE681, 0x2640,
    0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0, 0x2080, 0xE041,
    0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281, 0x6240,
    0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41,
    0xAA01, 0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840,
    0x7800, 0xB8C1, 0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41,
    0xBE01, 0x7EC0, 0x7F80, 0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40,
    0xB401, 0x74C0, 0x7580, 0xB541, 0x7700, 0xB7C1, 0xB681, 0x7640,
    0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101, 0x71C0, 0x7080, 0xB041,
    0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0, 0x5280, 0x9241,
    0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481, 0x5440,
    0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841,
    0x8801, 0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40,
    0x4E00, 0x8EC1, 0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41,
    0x4400, 0x84C1, 0x8581, 0x4540, 0x8701, 0x47C0, 0x4680, 0x8641,
    0x8201, 0x42C0, 0x4380, 0x8341, 0x4100, 0x81C1, 0x8081, 0x4040};

static uint16_t recordCrc(const uint8_t* p, size_t n) {
  uint16_t c = 0xffff;
  while (n--) {
    c = static_cast<uint16_t>((c >> 8) ^
                              CRC16_MODBUS_TABLE[(c ^ *p++) & 0xFFU]);
  }
  return c;
}

// This is the exact 12-byte core written by bridge firmware v2. Keep it
// separate from StoredRecord: embedding the enlarged v3 record would move the
// raw sequence, extension, and CRC offsets and make deployed flash unreadable.
struct __attribute__((packed)) DiskLegacyCore {
  uint32_t time_s;
  uint16_t seq;
  int16_t temp_centi;
  uint16_t voltage_mv;
  int8_t rssi;
  uint8_t flags;
};
static_assert(sizeof(DiskLegacyCore) == 12, "legacy disk core size");

struct __attribute__((packed)) FlashRecord {
  uint32_t magic;
  uint32_t generation;
  DiskLegacyCore core;
  uint8_t raw_seq;
  uint8_t extension[9];
  uint16_t crc;
};
static_assert(sizeof(FlashRecord) == 32, "FlashRecord alignment");
static_assert(offsetof(FlashRecord, magic) == 0, "flash magic offset");
static_assert(offsetof(FlashRecord, generation) == 4,
              "flash generation offset");
static_assert(offsetof(FlashRecord, core) == 8, "flash core offset");
static_assert(offsetof(FlashRecord, raw_seq) == 20,
              "flash raw sequence offset");
static_assert(offsetof(FlashRecord, extension) == 21,
              "flash extension offset");
static_assert(offsetof(FlashRecord, crc) == 30, "flash CRC offset");
static const uint8_t FLASH_FORMAT_LEGACY = 0;
static const uint8_t FLASH_FORMAT_V3 = 3;
static const uint8_t FLASH_EXTENSION_FORMAT_OFFSET = 4;
static const uint32_t FLASH_MAGIC = 0x33435242UL;  // BRC3
static const uint32_t FLASH_SECTOR_SIZE = 4096;
static const uint32_t PRIMARY_REGION_SIZE = 256UL * 1024UL;
static const uint32_t FLASH_SLOTS = PRIMARY_REGION_SIZE / sizeof(FlashRecord);
static const uint32_t PATCH_REGION_OFFSET = PRIMARY_REGION_SIZE;
static const uint32_t PATCH_REGION_SIZE = 256UL * 1024UL;
static const uint32_t PATCH_MAGIC = 0x33484342UL;  // BCH3

struct __attribute__((packed)) EpochPatch {
  uint32_t magic;
  uint32_t generation;
  uint32_t epoch_s;
  uint16_t crc;
  uint16_t reserved;
};
static_assert(sizeof(EpochPatch) == 16, "EpochPatch alignment");
static const uint32_t PATCH_SLOTS = PATCH_REGION_SIZE / sizeof(EpochPatch);
static const uint8_t PENDING_RECORD_CAPACITY = 8;

static uint32_t storedRecordEpoch(const StoredRecord& record,
                                  const TimeSnapshot& clock) {
  if ((record.flags & RECORD_FLAG_EPOCH_VALID) != 0) {
    return record.time_s;
  }
  if ((record.flags & RECORD_FLAG_PREVIOUS_BOOT) != 0) {
    return 0;
  }
  return epochForUptime(record.time_s, clock);
}

static Adafruit_FlashTransport_QSPI flashTransport;
static Adafruit_SPIFlash flash(&flashTransport, false);
static const SPIFlash_Device_t P25Q16 = {
  .total_size = (1UL << 21), .start_up_time_us = 5000,
  .manufacturer_id = 0x85, .memory_type = 0x60, .capacity = 0x15,
  .max_clock_speed_mhz = 32, .quad_enable_bit_mask = 0x02,
  .has_sector_protection = true, .supports_fast_read = true,
  .supports_qspi = true, .supports_qspi_writes = true,
  .write_status_register_split = true, .single_status_byte = false,
  .is_fram = false};

class PersistentRecordStore {
 public:
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == NULL) {
      return false;
    }
    if (!flash.begin(&P25Q16, 1)) {
      Serial.println("[storage] QSPI init failed");
      return false;
    }

    resetStateLocked();
    erasing_ = false;

    // One pass over the base log finds the newest valid record, the next
    // physical write slot, and the ring contents. The previous two passes read
    // and CRC-checked the whole 256 KiB region twice; a valid slot can never
    // carry a generation above generation_, so keeping the highest generation
    // seen per ring index and pruning afterwards selects exactly the same
    // records the second pass used to select.
    FlashRecord fr;
    StoredRecord decoded;
    for (uint32_t i = 0; i < FLASH_SLOTS; ++i) {
      if (!readFlashRecord(i, fr, decoded)) {
        continue;
      }
      if (!have_last_ || generationNewer(fr.generation, generation_)) {
        generation_ = fr.generation;
        flash_slot_ = static_cast<uint16_t>((i + 1U) % FLASH_SLOTS);
        have_last_ = true;
        last_raw_ = fr.raw_seq;
        extended_ = decoded.seq;
      }

      const uint16_t ri = static_cast<uint16_t>(fr.generation % RECORD_CAPACITY);
      if (generations_[ri] == 0 || fr.generation > generations_[ri]) {
        records_[ri] = decoded;
        generations_[ri] = fr.generation;
      }
    }

    // Retain only the newest logical 4096 records. Physical overprovisioning
    // guarantees that erasing the next sector cannot remove this window.
    if (have_last_) {
      const uint32_t first = firstRetainedGeneration();
      for (uint32_t ri = 0; ri < RECORD_CAPACITY; ++ri) {
        const uint32_t g = generations_[ri];
        if (g == 0) {
          continue;
        }
        if (g < first || g > generation_) {
          generations_[ri] = 0;
          memset(&records_[ri], 0, sizeof(records_[ri]));
        } else {
          ++count_;
        }
      }
    }
    // With have_last_ false no slot decoded successfully, so nothing was ever
    // written into the ring and there is nothing to prune.

    // Epoch patches are independent of the base log. A torn patch is ignored;
    // a valid patch is applied only to its exact logical generation.
    EpochPatch patch;
    for (uint32_t i = 0; i < PATCH_SLOTS; ++i) {
      if (!readEpochPatch(i, patch)) {
        continue;
      }
      if (!have_patch_ || generationNewer(patch.generation,
                                          latest_patch_generation_)) {
        latest_patch_generation_ = patch.generation;
        patch_slot_ = static_cast<uint16_t>((i + 1U) % PATCH_SLOTS);
        have_patch_ = true;
      }
      const uint16_t ri = static_cast<uint16_t>(patch.generation % RECORD_CAPACITY);
      if (generations_[ri] == patch.generation) {
        records_[ri].time_s = patch.epoch_s;
        records_[ri].flags = static_cast<uint8_t>(
            (records_[ri].flags | RECORD_FLAG_EPOCH_VALID) &
            ~RECORD_FLAG_PREVIOUS_BOOT);
      }
    }

    // Any unpatched uptime loaded from flash belongs to a previous boot. It is
    // retained, but its timestamp is explicitly unknown rather than being
    // reinterpreted with this boot's future time-sync offset.
    if (have_last_) {
      const uint32_t first = firstRetainedGeneration();
      for (uint32_t g = first; g <= generation_; ++g) {
        const uint16_t ri = static_cast<uint16_t>(g % RECORD_CAPACITY);
        if (generations_[ri] == g &&
            (records_[ri].flags & RECORD_FLAG_EPOCH_VALID) == 0) {
          records_[ri].flags |= RECORD_FLAG_PREVIOUS_BOOT;
        }
      }
    }
    boot_generation_ = generation_;
    return true;
  }

  void append(const StoredRecord& record, uint8_t raw_seq) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (erasing_) {
      xSemaphoreGive(mutex_);
      Serial.println("[storage] sample dropped while history erase is active");
      return;
    }
    FlashRecord fr = {};
    fr.magic = FLASH_MAGIC;
    fr.generation = ++generation_;
    fr.core.time_s = record.time_s;
    fr.core.seq = record.seq;
    fr.core.temp_centi = record.temp_centi;
    fr.core.voltage_mv = record.voltage_mv;
    fr.core.rssi = record.rssi;
    fr.core.flags = record.flags;
    fr.raw_seq = raw_seq;
    writeLe16(fr.extension + 0, record.lux_x1);
    writeLe16(fr.extension + 2, record.hum_centi);
    fr.extension[FLASH_EXTENSION_FORMAT_OFFSET] = FLASH_FORMAT_V3;
    fr.crc = recordCrc(reinterpret_cast<const uint8_t*>(&fr), sizeof(fr) - 2);
    const uint16_t ri = static_cast<uint16_t>(fr.generation % RECORD_CAPACITY);
    const bool replaced_retained =
        generations_[ri] != 0 &&
        generations_[ri] + RECORD_CAPACITY == fr.generation;
    records_[ri] = record;
    generations_[ri] = fr.generation;
    if (!replaced_retained && count_ < RECORD_CAPACITY) {
      ++count_;
    }
    if (pending_count_ < PENDING_RECORD_CAPACITY) {
      pending_[pending_tail_] = fr;
      pending_tail_ = static_cast<uint8_t>(
          (pending_tail_ + 1U) % PENDING_RECORD_CAPACITY);
      ++pending_count_;
    } else {
      storage_fault_ = true;
      Serial.println("[storage] pending FIFO full; sample remains RAM-only");
    }
    have_last_ = true;
    last_raw_ = raw_seq;
    extended_ = record.seq;
    xSemaphoreGive(mutex_);
  }

  uint32_t count() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const uint32_t result = count_;
    xSemaphoreGive(mutex_);
    return result;
  }

  bool lastRaw(uint8_t& raw, uint16_t& ext) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool result = have_last_;
    raw = last_raw_;
    ext = extended_;
    xSemaphoreGive(mutex_);
    return result;
  }

  void scheduleEpochPatches(const TimeSnapshot& clock) {
    if (!clock.synced) {
      return;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!patch_scan_active_) {
      patch_scan_generation_ = boot_generation_ + 1U;
      patch_target_generation_ = generation_;
      patch_clock_ = clock;
      patch_scan_active_ = patch_scan_generation_ <= patch_target_generation_;
    }
    xSemaphoreGive(mutex_);
  }

  void service() {
    enum WorkType { WORK_NONE, WORK_PRIMARY, WORK_PATCH };
    WorkType work = WORK_NONE;
    FlashRecord primary = {};
    EpochPatch patch = {};
    uint32_t address = 0;

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (erasing_) {
      xSemaphoreGive(mutex_);
      return;
    }
    if (pending_count_ != 0) {
      primary = pending_[pending_head_];
      address = static_cast<uint32_t>(flash_slot_) * sizeof(FlashRecord);
      work = WORK_PRIMARY;
    } else if (prepareNextPatchLocked(patch)) {
      address = PATCH_REGION_OFFSET +
                static_cast<uint32_t>(patch_slot_) * sizeof(EpochPatch);
      work = WORK_PATCH;
    }
    xSemaphoreGive(mutex_);

    if (work == WORK_NONE) {
      return;
    }

    // Flash I/O happens with no record-store mutex held. BLE callbacks can
    // continue accepting samples while a sector erase or page program runs.
    bool ok = true;
    if (address % FLASH_SECTOR_SIZE == 0) {
      ok = flash.eraseSector(address / FLASH_SECTOR_SIZE);
    }
    if (ok) {
      const uint8_t* bytes = work == WORK_PRIMARY
                                 ? reinterpret_cast<const uint8_t*>(&primary)
                                 : reinterpret_cast<const uint8_t*>(&patch);
      const size_t length = work == WORK_PRIMARY ? sizeof(primary) : sizeof(patch);
      ok = flash.writeBuffer(address, bytes, length) == length;
      if (ok) {
        flash.waitUntilReady();
      }
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (ok && work == WORK_PRIMARY && pending_count_ != 0 &&
        pending_[pending_head_].generation == primary.generation) {
      flash_slot_ = static_cast<uint16_t>((flash_slot_ + 1U) % FLASH_SLOTS);
      pending_head_ = static_cast<uint8_t>(
          (pending_head_ + 1U) % PENDING_RECORD_CAPACITY);
      --pending_count_;
    } else if (ok && work == WORK_PATCH) {
      patch_slot_ = static_cast<uint16_t>((patch_slot_ + 1U) % PATCH_SLOTS);
      const uint16_t ri = static_cast<uint16_t>(patch.generation % RECORD_CAPACITY);
      if (generations_[ri] == patch.generation) {
        records_[ri].time_s = patch.epoch_s;
        records_[ri].flags = static_cast<uint8_t>(
            (records_[ri].flags | RECORD_FLAG_EPOCH_VALID) &
            ~RECORD_FLAG_PREVIOUS_BOOT);
      }
      if (patch_scan_generation_ == patch.generation) {
        ++patch_scan_generation_;
      }
    } else if (!ok) {
      storage_fault_ = true;
      Serial.println("[storage] deferred flash operation failed; retrying");
    }
    xSemaphoreGive(mutex_);
  }

  bool eraseAll() {
    // Cancel all queued writes and patch discovery before touching flash.
    // Scanner callbacks remain responsive, but append() deliberately drops
    // samples while the physical erase is in progress.
    xSemaphoreTake(mutex_, portMAX_DELAY);
    erasing_ = true;
    pending_head_ = 0;
    pending_tail_ = 0;
    pending_count_ = 0;
    patch_scan_active_ = false;
    xSemaphoreGive(mutex_);

    const uint32_t started_ms = millis();
    const uint32_t erased_region_size =
        PRIMARY_REGION_SIZE + PATCH_REGION_SIZE;
    const uint32_t sector_count =
        erased_region_size / FLASH_SECTOR_SIZE;
    bool ok = true;
    uint32_t erased_sectors = 0;
    for (uint32_t address = 0; address < erased_region_size;
         address += FLASH_SECTOR_SIZE) {
      if (!flash.eraseSector(address / FLASH_SECTOR_SIZE)) {
        ok = false;
        Serial.printf("[erase] sector %lu failed\n",
                      static_cast<unsigned long>(
                          address / FLASH_SECTOR_SIZE));
      } else {
        flash.waitUntilReady();
        ++erased_sectors;
      }
      yield();
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (ok) {
      resetStateLocked();
      erasing_ = false;
    } else {
      storage_fault_ = true;
      erasing_ = false;
    }
    xSemaphoreGive(mutex_);

    Serial.printf("[erase] %s sectors=%lu/%lu elapsed=%lu ms\n",
                  ok ? "complete" : "failed",
                  static_cast<unsigned long>(erased_sectors),
                  static_cast<unsigned long>(sector_count),
                  static_cast<unsigned long>(millis() - started_ms));
    return ok;
  }

  bool latest(StoredRecord& record) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (count_ == 0) {
      xSemaphoreGive(mutex_);
      return false;
    }

    const uint16_t index = static_cast<uint16_t>(generation_ % RECORD_CAPACITY);
    record = records_[index];
    xSemaphoreGive(mutex_);
    return true;
  }

  void statusSnapshot(uint32_t& count, bool& has_latest,
                      StoredRecord& latest_record) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    count = count_;
    has_latest = count_ != 0;
    if (has_latest) {
      const uint16_t index = static_cast<uint16_t>(generation_ % RECORD_CAPACITY);
      latest_record = records_[index];
    }
    xSemaphoreGive(mutex_);
  }

  // Produces a stable oldest-first snapshot.  Holding the mutex while copying
  // prevents a full ring from overwriting an unsent record; 64 KiB is copied
  // quickly, and the sensor callback only waits for this short memory pass.
  size_t iterateSince(uint32_t since_epoch, const TimeSnapshot& clock,
                      StoredRecord* destination, size_t destination_capacity) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    size_t written = 0;
    const uint32_t first = firstRetainedGeneration();
    for (uint32_t g = first;
         g <= generation_ && written < destination_capacity; ++g) {
      const uint16_t index = static_cast<uint16_t>(g % RECORD_CAPACITY);
      if (generations_[index] != g) {
        continue;
      }
      const StoredRecord& record = records_[index];
      const uint32_t epoch = storedRecordEpoch(record, clock);
      // An unsynced prior-boot record has no comparable epoch, so always
      // include it. The web app deduplicates the repeated sequence key.
      if (since_epoch == 0 || epoch == 0 || epoch >= since_epoch) {
        destination[written++] = record;
      }
    }

    xSemaphoreGive(mutex_);
    return written;
  }

 private:
  void resetStateLocked() {
    memset(records_, 0, sizeof(records_));
    memset(generations_, 0, sizeof(generations_));
    memset(pending_, 0, sizeof(pending_));
    count_ = 0;
    generation_ = 0;
    boot_generation_ = 0;
    latest_patch_generation_ = 0;
    patch_scan_generation_ = 0;
    patch_target_generation_ = 0;
    flash_slot_ = 0;
    patch_slot_ = 0;
    have_last_ = false;
    have_patch_ = false;
    storage_fault_ = false;
    patch_scan_active_ = false;
    last_raw_ = 0;
    extended_ = 0;
    patch_clock_ = {};
    pending_head_ = 0;
    pending_tail_ = 0;
    pending_count_ = 0;
  }

  static bool generationNewer(uint32_t candidate, uint32_t reference) {
    return static_cast<int32_t>(candidate - reference) > 0;
  }

  uint32_t firstRetainedGeneration() const {
    return generation_ > RECORD_CAPACITY
               ? generation_ - static_cast<uint32_t>(RECORD_CAPACITY) + 1U
               : 1U;
  }

  static bool decodeFlashRecord(const FlashRecord& disk,
                                StoredRecord& record) {
    const uint8_t format =
        disk.extension[FLASH_EXTENSION_FORMAT_OFFSET];
    if (format == FLASH_FORMAT_LEGACY) {
      // Firmware v2 value-initialized all nine reserved bytes. Requiring that
      // exact layout prevents a damaged v3 extension from being mistaken for
      // an old record even if its marker byte happens to become zero.
      for (size_t i = 0; i < sizeof(disk.extension); ++i) {
        if (disk.extension[i] != 0) {
          return false;
        }
      }
      record.lux_x1 = INVALID_U16;
      record.hum_centi = INVALID_U16;
      record.flags = static_cast<uint8_t>(
          disk.core.flags &
          (RECORD_FLAG_CLOCK_SYNCED | RECORD_FLAG_PREVIOUS_BOOT |
           RECORD_FLAG_EPOCH_VALID));
    } else if (format == FLASH_FORMAT_V3) {
      for (size_t i = 5; i < sizeof(disk.extension); ++i) {
        if (disk.extension[i] != 0) {
          return false;
        }
      }
      record.lux_x1 = readLe16(disk.extension + 0);
      record.hum_centi = readLe16(disk.extension + 2);
      record.flags = disk.core.flags;

      const uint8_t configuration = static_cast<uint8_t>(
          (record.flags & RECORD_FLAG_CONFIG_MASK) >>
          RECORD_FLAG_CONFIG_SHIFT);
      if (configuration > 4 ||
          ((record.flags & RECORD_FLAG_VSTOR_VALID) != 0 &&
           configuration != 4)) {
        return false;
      }
    } else {
      return false;
    }

    record.time_s = disk.core.time_s;
    record.seq = disk.core.seq;
    record.temp_centi = disk.core.temp_centi;
    record.voltage_mv = disk.core.voltage_mv;
    record.rssi = disk.core.rssi;
    return true;
  }

  bool readFlashRecord(uint32_t slot, FlashRecord& disk,
                       StoredRecord& record) {
    if (flash.readBuffer(slot * sizeof(FlashRecord),
                         reinterpret_cast<uint8_t*>(&disk), sizeof(disk)) !=
        sizeof(disk)) {
      return false;
    }
    return disk.magic == FLASH_MAGIC && disk.generation != 0 &&
           recordCrc(reinterpret_cast<const uint8_t*>(&disk),
                     sizeof(disk) - 2) == disk.crc &&
           decodeFlashRecord(disk, record);
  }

  bool readEpochPatch(uint32_t slot, EpochPatch& patch) {
    if (flash.readBuffer(PATCH_REGION_OFFSET + slot * sizeof(EpochPatch),
                         reinterpret_cast<uint8_t*>(&patch), sizeof(patch)) !=
        sizeof(patch)) {
      return false;
    }
    return patch.magic == PATCH_MAGIC && patch.generation != 0 &&
           recordCrc(reinterpret_cast<const uint8_t*>(&patch), 12) == patch.crc;
  }

  bool prepareNextPatchLocked(EpochPatch& patch) {
    while (patch_scan_active_ &&
           patch_scan_generation_ <= patch_target_generation_) {
      const uint32_t generation = patch_scan_generation_;
      const uint16_t ri = static_cast<uint16_t>(generation % RECORD_CAPACITY);
      if (generations_[ri] != generation ||
          (records_[ri].flags & RECORD_FLAG_EPOCH_VALID) != 0 ||
          (records_[ri].flags & RECORD_FLAG_PREVIOUS_BOOT) != 0) {
        ++patch_scan_generation_;
        continue;
      }
      patch = {};
      patch.magic = PATCH_MAGIC;
      patch.generation = generation;
      patch.epoch_s = epochForUptime(records_[ri].time_s, patch_clock_);
      patch.crc = recordCrc(reinterpret_cast<const uint8_t*>(&patch), 12);
      return true;
    }
    patch_scan_active_ = false;
    return false;
  }

  StoredRecord records_[RECORD_CAPACITY];
  uint32_t generations_[RECORD_CAPACITY] = {};
  SemaphoreHandle_t mutex_ = NULL;
  uint16_t count_ = 0;  // Number currently retained, at most 4096.
  uint32_t generation_ = 0;
  uint32_t boot_generation_ = 0;
  uint32_t latest_patch_generation_ = 0;
  uint32_t patch_scan_generation_ = 0;
  uint32_t patch_target_generation_ = 0;
  uint16_t flash_slot_ = 0;
  uint16_t patch_slot_ = 0;
  bool have_last_ = false;
  bool have_patch_ = false;
  bool storage_fault_ = false;
  bool patch_scan_active_ = false;
  bool erasing_ = false;
  uint8_t last_raw_ = 0;
  uint16_t extended_ = 0;
  TimeSnapshot patch_clock_ = {};
  FlashRecord pending_[PENDING_RECORD_CAPACITY] = {};
  uint8_t pending_head_ = 0;
  uint8_t pending_tail_ = 0;
  uint8_t pending_count_ = 0;
};

// ---------------------------------------------------------------------------
// Small cross-task mailboxes
// ---------------------------------------------------------------------------

struct HistoryRequest {
  uint16_t conn_handle;
  uint32_t since_epoch;
};

class HistoryRequestMailbox {
 public:
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != NULL;
  }

  // One queued request is enough for a single-client v1 bridge.  A newer write
  // replaces an older request that has not begun yet.
  bool post(const HistoryRequest& request) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool replaced = pending_;
    request_ = request;
    pending_ = true;
    xSemaphoreGive(mutex_);
    return replaced;
  }

  bool take(HistoryRequest& request) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!pending_) {
      xSemaphoreGive(mutex_);
      return false;
    }
    request = request_;
    pending_ = false;
    xSemaphoreGive(mutex_);
    return true;
  }

  void discardConnection(uint16_t conn_handle) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (pending_ && request_.conn_handle == conn_handle) {
      pending_ = false;
    }
    xSemaphoreGive(mutex_);
  }

  void clear() {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    pending_ = false;
    xSemaphoreGive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_ = NULL;
  bool pending_ = false;
  HistoryRequest request_ = {};
};

struct PendingLiveNotification {
  uint16_t conn_handle;
  uint32_t generation;
  uint8_t record[RECORD_SIZE];
};

class LiveNotificationMailbox {
 public:
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != NULL;
  }

  void post(uint16_t conn_handle, const uint8_t* record) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ++generation_;
    pending_.conn_handle = conn_handle;
    pending_.generation = generation_;
    memcpy(pending_.record, record, RECORD_SIZE);
    has_pending_ = true;
    xSemaphoreGive(mutex_);
  }

  bool peek(PendingLiveNotification& notification) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!has_pending_) {
      xSemaphoreGive(mutex_);
      return false;
    }
    notification = pending_;
    xSemaphoreGive(mutex_);
    return true;
  }

  void complete(uint32_t generation) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (has_pending_ && pending_.generation == generation) {
      has_pending_ = false;
    }
    xSemaphoreGive(mutex_);
  }

  void discardConnection(uint16_t conn_handle) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (has_pending_ && pending_.conn_handle == conn_handle) {
      has_pending_ = false;
    }
    xSemaphoreGive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_ = NULL;
  bool has_pending_ = false;
  uint32_t generation_ = 0;
  PendingLiveNotification pending_ = {};
};

struct PendingStatusNotification {
  uint16_t conn_handle;
  uint32_t generation;
  uint8_t status[STATUS_SIZE];
};

class StatusNotificationMailbox {
 public:
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != NULL;
  }

  void post(uint16_t conn_handle, const uint8_t* status) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    ++generation_;
    pending_.conn_handle = conn_handle;
    pending_.generation = generation_;
    memcpy(pending_.status, status, STATUS_SIZE);
    has_pending_ = true;
    xSemaphoreGive(mutex_);
  }

  bool peek(PendingStatusNotification& notification) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!has_pending_) {
      xSemaphoreGive(mutex_);
      return false;
    }
    notification = pending_;
    xSemaphoreGive(mutex_);
    return true;
  }

  void complete(uint32_t generation) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (has_pending_ && pending_.generation == generation) {
      has_pending_ = false;
    }
    xSemaphoreGive(mutex_);
  }

  void discardConnection(uint16_t conn_handle) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (has_pending_ && pending_.conn_handle == conn_handle) {
      has_pending_ = false;
    }
    xSemaphoreGive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_ = NULL;
  bool has_pending_ = false;
  uint32_t generation_ = 0;
  PendingStatusNotification pending_ = {};
};

class SensorMetadataState {
 public:
  bool begin() {
    mutex_ = xSemaphoreCreateMutex();
    return mutex_ != NULL;
  }

  bool update(uint8_t configuration, uint32_t advertised_interval_ms) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    const bool changed =
        configuration_ != configuration ||
        advertised_interval_ms_ != advertised_interval_ms;
    configuration_ = configuration;
    advertised_interval_ms_ = advertised_interval_ms;
    xSemaphoreGive(mutex_);
    return changed;
  }

  void snapshot(uint8_t& configuration,
                uint32_t& advertised_interval_ms) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    configuration = configuration_;
    advertised_interval_ms = advertised_interval_ms_;
    xSemaphoreGive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_ = NULL;
  uint8_t configuration_ = 0;
  uint32_t advertised_interval_ms_ = 0;
};

// ---------------------------------------------------------------------------
// BLE objects and shared state
// ---------------------------------------------------------------------------

BLEService bridgeService(SERVICE_UUID);
BLECharacteristic liveCharacteristic(LIVE_UUID);
BLECharacteristic historyControlCharacteristic(HIST_CTRL_UUID);
BLECharacteristic historyDataCharacteristic(HIST_DATA_UUID);
BLECharacteristic timeSyncCharacteristic(TIMESYNC_UUID);
BLECharacteristic statusCharacteristic(STATUS_UUID);

BridgeClock bridgeClock;
PersistentRecordStore recordStore;
HistoryRequestMailbox historyRequests;
LiveNotificationMailbox liveNotifications;
StatusNotificationMailbox statusNotifications;
SensorMetadataState sensorMetadata;

// A stable history snapshot prevents concurrent ring overwrites during a long
// transfer. It is another 64 KiB, still comfortably inside nRF52840 RAM.
StoredRecord historySnapshot[RECORD_CAPACITY];

struct HistoryStreamState {
  bool active;
  uint16_t conn_handle;
  uint32_t since_epoch;
  size_t record_count;
  size_t next_record;
  size_t last_progress_log;
  TimeSnapshot clock;
};

HistoryStreamState historyStream = {};

volatile uint16_t peripheralConnHandle = BLE_CONN_HANDLE_INVALID;

bool haveLastSensorSeq = false;
uint8_t lastSensorSeq = 0;
uint16_t extendedSensorSeq = 0;
uint64_t lastSensorSampleUptimeMs = 0;
bool haveLastSensorSampleUptime = false;
uint32_t legacyCadenceObservationsMs[LEGACY_CADENCE_WINDOW] = {};
uint8_t legacyCadenceObservationCount = 0;
uint8_t legacyCadenceObservationNext = 0;
volatile bool scanResumePending = false;
volatile bool eraseRequested = false;
volatile bool eraseInProgress = false;
volatile uint16_t eraseRequestConnHandle = BLE_CONN_HANDLE_INVALID;

// ---------------------------------------------------------------------------
// Wire encoding and sensor advertisement parsing
// ---------------------------------------------------------------------------

struct ParsedSensorSample {
  uint8_t seq;
  int16_t temp_centi;
  uint16_t voltage_mv;
  uint16_t lux_x1;
  uint16_t hum_centi;
  uint32_t sample_interval_ms;
  uint8_t configuration;
  bool has_reported_interval;
  bool hat_layout;
};

static void encodeRecord(const StoredRecord& record,
                         const TimeSnapshot& clock, uint8_t* output);
static bool parseBthomeServiceData(const uint8_t* data, size_t length,
                                   ParsedSensorSample& sample);
static bool parseSensorAdvertisement(const ble_gap_evt_adv_report_t* report,
                                     ParsedSensorSample& sample);
static void startHistory(const HistoryRequest& request);

static_assert(LEGACY_SERVICE_DATA_SIZE == 2 + 1 + 2 + 3 + 3,
              "legacy service-data size");
static_assert(BARE_SERVICE_DATA_SIZE ==
                  2 + 1 + 2 + 3 + 3 + 5 + 3,
              "enhanced bare service-data size");
static_assert(HAT_SERVICE_DATA_SIZE ==
                  2 + 1 + 2 + 3 + 3 + 4 + 3 + 5 + 3,
              "HAT service-data size");

static void formatPeerAddress(const ble_gap_addr_t& address,
                              char output[18]) {
  // SoftDevice stores the least-significant address byte first. Print the
  // conventional human-readable order used by nRF Connect.
  snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           address.addr[5], address.addr[4], address.addr[3],
           address.addr[2], address.addr[1], address.addr[0]);
}

static bool sensorAddressAllowed(const ble_gap_addr_t& address) {
  if (!SENSOR_ADDRESS_FILTER_ENABLED) {
    return true;
  }

  for (size_t i = 0; i < sizeof(SENSOR_ADDRESS); ++i) {
    if (address.addr[5U - i] != SENSOR_ADDRESS[i]) {
      return false;
    }
  }
  return true;
}

static void encodeRecord(const StoredRecord& record,
                         const TimeSnapshot& clock,
                         uint8_t* output) {
  writeLe32(output + 0, storedRecordEpoch(record, clock));
  writeLe16(output + 4, record.seq);
  writeLe16(output + 6, static_cast<uint16_t>(record.temp_centi));
  writeLe16(output + 8, record.voltage_mv);
  writeLe16(output + 10, record.lux_x1);
  writeLe16(output + 12, record.hum_centi);
  output[14] = static_cast<uint8_t>(record.rssi);
  output[15] = static_cast<uint8_t>(record.flags & RECORD_WIRE_FLAG_MASK);
}

static bool bthomeObjectValueSize(uint8_t object_id,
                                  const uint8_t* value,
                                  size_t remaining,
                                  size_t& value_size) {
  // Fixed sizes from the BTHome v2 object registry. This lets a newer sensor
  // add unrelated standard measurements without shifting or invalidating the
  // bridge's required core. IDs not listed here have an unknown encoding for
  // this firmware and are rejected because they cannot be skipped safely.
  if ((object_id >= 0x15 && object_id <= 0x2F)) {
    value_size = 1;
    return true;
  }
  switch (object_id) {
    case 0x00:
    case 0x01:
    case 0x09:
    case 0x0F:
    case 0x10:
    case 0x11:
    case 0x46:
    case 0x57:
    case 0x58:
    case 0x59:
    case 0x60:
    case 0x64:
    case 0x65:
      value_size = 1;
      return true;
    case 0x02:
    case 0x03:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x3C:
    case 0x3D:
    case 0x3F:
    case 0x40:
    case 0x41:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x51:
    case 0x52:
    case 0x56:
    case 0x5A:
    case 0x5D:
    case 0x5E:
    case 0x5F:
    case 0x61:
    case 0xF0:
      value_size = 2;
      return true;
    case 0x04:
    case 0x05:
    case 0x0A:
    case 0x0B:
    case 0x42:
    case 0x4B:
    case 0xF2:
      value_size = 3;
      return true;
    case 0x3E:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F:
    case 0x50:
    case 0x55:
    case 0x5B:
    case 0x5C:
    case 0x62:
    case 0x63:
    case 0xF1:
      value_size = 4;
      return true;
    case 0x3A:
      value_size = 1;
      return true;
    case 0x3B:
      if (remaining < 2 || (value[0] & 0xE0U) != 0) {
        return false;
      }
      value_size = 2U + (value[0] & 0x1FU);
      return true;
    case 0x53:
    case 0x54:
      if (remaining < 1) {
        return false;
      }
      value_size = 1U + value[0];
      return true;
    default:
      return false;
  }
}

static bool parseBthomeServiceData(const uint8_t* data, size_t length,
                                   ParsedSensorSample& sample) {
  if (data == NULL || length < LEGACY_SERVICE_DATA_SIZE ||
      length > MAX_SERVICE_DATA_SIZE ||
      readLe16(data) != BTHOME_UUID || data[2] != BTHOME_DEVICE_INFO) {
    return false;
  }

  enum SeenObject : uint8_t {
    SEEN_PACKET = 0x01,
    SEEN_TEMPERATURE = 0x02,
    SEEN_HUMIDITY = 0x04,
    SEEN_ILLUMINANCE = 0x08,
    SEEN_VOLTAGE = 0x10,
    SEEN_DURATION = 0x20,
    SEEN_CONFIGURATION = 0x40,
  };
  static const uint8_t REQUIRED =
      SEEN_PACKET | SEEN_TEMPERATURE | SEEN_VOLTAGE;

  sample = {};
  sample.voltage_mv = INVALID_U16;
  sample.lux_x1 = INVALID_U16;
  sample.hum_centi = INVALID_U16;

  uint8_t seen = 0;
  uint32_t illuminance_centi = 0xFFFFFFUL;
  const uint8_t* cursor = data + 3;
  size_t remaining = length - 3;
  while (remaining != 0) {
    const uint8_t object_id = *cursor++;
    --remaining;

    size_t value_size = 0;
    if (!bthomeObjectValueSize(object_id, cursor, remaining, value_size) ||
        remaining < value_size) {
      return false;
    }

    uint8_t seen_bit = 0;
    switch (object_id) {
      case BTHOME_PACKET_ID:
        seen_bit = SEEN_PACKET;
        break;
      case BTHOME_TEMPERATURE_ID:
        seen_bit = SEEN_TEMPERATURE;
        break;
      case BTHOME_HUMIDITY_ID:
        seen_bit = SEEN_HUMIDITY;
        break;
      case BTHOME_ILLUMINANCE_ID:
        seen_bit = SEEN_ILLUMINANCE;
        break;
      case BTHOME_VOLTAGE_ID:
        seen_bit = SEEN_VOLTAGE;
        break;
      case BTHOME_DURATION_ID:
        seen_bit = SEEN_DURATION;
        break;
      case BTHOME_RAW_ID:
        if (value_size != 4U || cursor[0] != 3U) {
          return false;
        }
        seen_bit = SEEN_DURATION;
        break;
      case BTHOME_DEVICE_TYPE_ID:
        seen_bit = SEEN_CONFIGURATION;
        break;
      default:
        break;  // Known unrelated object; bounds checked and safely skipped.
    }
    if (seen_bit != 0 && (seen & seen_bit) != 0) {
      return false;
    }
    seen |= seen_bit;

    switch (object_id) {
      case BTHOME_PACKET_ID:
        sample.seq = cursor[0];
        break;
      case BTHOME_TEMPERATURE_ID:
        sample.temp_centi =
            static_cast<int16_t>(readLe16(cursor));
        break;
      case BTHOME_HUMIDITY_ID:
        sample.hum_centi = readLe16(cursor);
        break;
      case BTHOME_ILLUMINANCE_ID:
        illuminance_centi = readLe24(cursor);
        break;
      case BTHOME_VOLTAGE_ID:
        sample.voltage_mv = readLe16(cursor);
        break;
      case BTHOME_DURATION_ID:
        sample.sample_interval_ms = readLe24(cursor);
        break;
      case BTHOME_RAW_ID:
        sample.sample_interval_ms = readLe24(cursor + 1);
        break;
      case BTHOME_DEVICE_TYPE_ID: {
        const uint16_t configuration = readLe16(cursor);
        if (configuration > 4) {
          return false;
        }
        sample.configuration = static_cast<uint8_t>(configuration);
        break;
      }
      default:
        break;
    }
    cursor += value_size;
    remaining -= value_size;
  }

  if ((seen & REQUIRED) != REQUIRED) {
    return false;
  }

  const bool has_environment =
      (seen & (SEEN_HUMIDITY | SEEN_ILLUMINANCE)) != 0;
  const bool has_metadata =
      (seen & (SEEN_DURATION | SEEN_CONFIGURATION)) != 0;
  if (!has_environment && !has_metadata) {
    if ((seen & REQUIRED) != REQUIRED) {
      return false;
    }
    sample.configuration = 0;
    sample.sample_interval_ms = 0;
    sample.has_reported_interval = false;
    sample.hat_layout = false;
    return true;
  }

  if (sample.sample_interval_ms == 0 || sample.configuration == 0) {
    return false;
  }
  sample.has_reported_interval = true;

  const uint8_t bare_objects =
      REQUIRED | SEEN_DURATION | SEEN_CONFIGURATION;
  if (!has_environment) {
    if ((seen & bare_objects) != bare_objects ||
        (sample.configuration != 1 && sample.configuration != 2)) {
      return false;
    }
    sample.hat_layout = false;
    return true;
  }

  const uint8_t hat_objects =
      bare_objects | SEEN_HUMIDITY | SEEN_ILLUMINANCE;
  if ((seen & hat_objects) != hat_objects ||
      (sample.configuration != 3 && sample.configuration != 4) ||
      (sample.hum_centi != INVALID_U16 && sample.hum_centi > 10000U)) {
    return false;
  }
  sample.hat_layout = true;

  if (illuminance_centi == 0xFFFFFFUL) {
    sample.lux_x1 = INVALID_U16;
  } else {
    uint32_t rounded_lux = (illuminance_centi + 50U) / 100U;
    if (rounded_lux >= INVALID_U16) {
      rounded_lux = INVALID_U16 - 1U;
    }
    sample.lux_x1 = static_cast<uint16_t>(rounded_lux);
  }

  // USB-powered HAT advertisements have no meaningful voltage channel. For
  // external-power mode, independently enforce the sensor's configured
  // plausibility band so malformed/out-of-range values cannot reach analytics.
  if (sample.configuration == 3 ||
      (sample.configuration == 4 &&
       (sample.voltage_mv < MIN_PLAUSIBLE_VSTOR_MV ||
        sample.voltage_mv > MAX_PLAUSIBLE_VSTOR_MV))) {
    sample.voltage_mv = INVALID_U16;
  }
  return true;
}

static bool parseSensorAdvertisement(const ble_gap_evt_adv_report_t* report,
                                     ParsedSensorSample& sample) {
  const uint8_t* cursor = report->data.p_data;
  size_t remaining = report->data.len;

  // Parse AD structures defensively instead of relying on a helper that trusts
  // the length byte.  A malformed ambient advertisement must never overrun the
  // scanner's shared report buffer.
  while (remaining != 0) {
    const uint8_t field_length = cursor[0];
    if (field_length == 0) {
      break;  // Zero is legal end padding in a legacy advertising payload.
    }
    if (static_cast<size_t>(field_length) + 1U > remaining) {
      return false;
    }

    const uint8_t ad_type = cursor[1];
    const size_t data_length = field_length - 1U;
    const uint8_t* data = cursor + 2;

    if (ad_type == BLE_GAP_AD_TYPE_SERVICE_DATA && data_length >= 2 &&
        readLe16(data) == BTHOME_UUID) {
      return parseBthomeServiceData(data, data_length, sample);
    }

    const size_t consumed = static_cast<size_t>(field_length) + 1U;
    cursor += consumed;
    remaining -= consumed;
  }

  return false;
}

static void resetLegacyCadence() {
  legacyCadenceObservationCount = 0;
  legacyCadenceObservationNext = 0;
  memset(legacyCadenceObservationsMs, 0,
         sizeof(legacyCadenceObservationsMs));
}

static void observeLegacyCadence(uint64_t elapsed_ms, uint8_t raw_delta) {
  if (raw_delta == 0 || raw_delta > MAX_SMALL_RESYNC_STEPS ||
      elapsed_ms == 0) {
    return;
  }
  const uint64_t per_sample =
      (elapsed_ms + raw_delta / 2U) / raw_delta;
  if (per_sample == 0 || per_sample > UINT32_MAX) {
    return;
  }
  legacyCadenceObservationsMs[legacyCadenceObservationNext] =
      static_cast<uint32_t>(per_sample);
  legacyCadenceObservationNext = static_cast<uint8_t>(
      (legacyCadenceObservationNext + 1U) % LEGACY_CADENCE_WINDOW);
  if (legacyCadenceObservationCount < LEGACY_CADENCE_WINDOW) {
    ++legacyCadenceObservationCount;
  }
}

static uint32_t learnedLegacyCadenceMs() {
  if (legacyCadenceObservationCount == 0) {
    return 0;
  }
  uint32_t sorted[LEGACY_CADENCE_WINDOW];
  memcpy(sorted, legacyCadenceObservationsMs,
         legacyCadenceObservationCount * sizeof(sorted[0]));
  for (uint8_t i = 1; i < legacyCadenceObservationCount; ++i) {
    const uint32_t value = sorted[i];
    uint8_t j = i;
    while (j != 0 && sorted[j - 1U] > value) {
      sorted[j] = sorted[j - 1U];
      --j;
    }
    sorted[j] = value;
  }
  return sorted[legacyCadenceObservationCount / 2U];
}

static void refreshLiveValue(bool queue_notification) {
  StoredRecord latest;
  if (!recordStore.latest(latest)) {
    return;
  }

  const TimeSnapshot clock = bridgeClock.snapshot();
  uint8_t encoded[RECORD_SIZE];
  encodeRecord(latest, clock, encoded);
  liveCharacteristic.write(encoded, sizeof(encoded));

  if (!queue_notification) {
    return;
  }

  const uint16_t conn_handle = peripheralConnHandle;
  if (conn_handle != BLE_CONN_HANDLE_INVALID &&
      Bluefruit.connected(conn_handle) &&
      liveCharacteristic.notifyEnabled(conn_handle)) {
    // Notify from loop(), not this scanner callback.  If the SoftDevice TX
    // queue is full, loop() retries without delaying Scanner.resume().
    liveNotifications.post(conn_handle, encoded);
  }
}

static void encodeStatus(const TimeSnapshot& clock, uint8_t* output) {
  uint32_t record_count = 0;
  bool has_latest = false;
  StoredRecord latest = {};
  recordStore.statusSnapshot(record_count, has_latest, latest);

  // With no retained sample (or no comparable timestamp), age is unknown.
  uint32_t last_sample_age_s = UINT32_MAX;
  int8_t last_rssi = 0;
  if (has_latest) {
    last_sample_age_s = UINT32_MAX;
    if ((latest.flags & RECORD_FLAG_EPOCH_VALID) != 0) {
      const uint32_t epoch = latest.time_s;
      const uint32_t now_epoch =
          epochForUptime(clock.uptime_s, clock);
      last_sample_age_s =
          (clock.synced && now_epoch >= epoch)
              ? now_epoch - epoch
              : UINT32_MAX;
    } else if ((latest.flags & RECORD_FLAG_PREVIOUS_BOOT) == 0 &&
               clock.uptime_s >= latest.time_s) {
      last_sample_age_s = clock.uptime_s - latest.time_s;
    }
    last_rssi = latest.rssi;
  }

  writeLe32(output + 0, record_count);
  writeLe32(output + 4, last_sample_age_s);
  output[8] = static_cast<uint8_t>(last_rssi);
  output[9] = clock.synced ? STATUS_FLAG_CLOCK_SYNCED : 0;
  output[10] = FW_VERSION;
  uint8_t configuration = 0;
  uint32_t advertised_interval_ms = 0;
  sensorMetadata.snapshot(configuration, advertised_interval_ms);
  output[11] = configuration;
  writeLe32(output + 12, advertised_interval_ms);
}

static void refreshStatusValue(bool queue_notification) {
  uint8_t encoded[STATUS_SIZE];
  encodeStatus(bridgeClock.snapshot(), encoded);
  statusCharacteristic.write(encoded, sizeof(encoded));

  if (!queue_notification) {
    return;
  }
  const uint16_t conn_handle = peripheralConnHandle;
  if (conn_handle != BLE_CONN_HANDLE_INVALID &&
      Bluefruit.connected(conn_handle) &&
      statusCharacteristic.notifyEnabled(conn_handle)) {
    statusNotifications.post(conn_handle, encoded);
  }
}

// ---------------------------------------------------------------------------
// BLE callbacks
// ---------------------------------------------------------------------------

void scanCallback(ble_gap_evt_adv_report_t* report) {
  ParsedSensorSample sample;
  const bool accepted = parseSensorAdvertisement(report, sample);
  const bool address_allowed =
      accepted && sensorAddressAllowed(report->peer_addr);
  const int8_t rssi = report->rssi;

  char peer_address[18] = {0};
  if (accepted) {
    formatPeerAddress(report->peer_addr, peer_address);
  }

  // Bluefruit pauses after every report.  Parsing is complete and all needed
  // fields have been copied, so resume immediately before storage/logging.
  if (!Bluefruit.Scanner.resume()) {
    if (!scanResumePending) {
      Serial.println("[scan] resume deferred; retrying from loop");
    }
    scanResumePending = true;
  }

  if (!accepted) {
    return;
  }
  if (!address_allowed) {
    Serial.printf("[scan] rejected BTHome advertiser=%s raw_seq=%u\n",
                  peer_address, static_cast<unsigned int>(sample.seq));
    return;
  }

  const TimeSnapshot clock = bridgeClock.snapshot();
  uint8_t previous_configuration = 0;
  uint32_t previous_advertised_interval_ms = 0;
  sensorMetadata.snapshot(previous_configuration,
                          previous_advertised_interval_ms);
  const bool previous_had_reported_interval =
      previous_configuration != 0 &&
      previous_advertised_interval_ms != 0;
  const bool cadence_source_changed =
      sample.has_reported_interval !=
      previous_had_reported_interval;
  if (cadence_source_changed) {
    resetLegacyCadence();
  }

  const uint32_t nominal_cadence_ms =
      sample.has_reported_interval
          ? sample.sample_interval_ms
          : learnedLegacyCadenceMs();
  const bool have_elapsed = haveLastSensorSampleUptime;
  const uint64_t elapsed_ms =
      have_elapsed ? clock.uptime_ms - lastSensorSampleUptimeMs : 0;

  // A packet ID repeats within each advertising burst, but it can also repeat
  // after a sensor reboot. Drop it only inside 1.5 nominal sample periods.
  // Without a reported or learned cadence there is not enough evidence to
  // classify the packet as a duplicate, so preserve it.
  if (haveLastSensorSeq && sample.seq == lastSensorSeq && have_elapsed &&
      nominal_cadence_ms != 0 &&
      elapsed_ms <
          static_cast<uint64_t>(nominal_cadence_ms) +
              nominal_cadence_ms / 2U) {
    return;
  }

  uint32_t skipped = 0;
  bool restartResync = false;
  bool fullWrapAmbiguous = false;
  uint64_t elapsedStepEstimate = 0;
  if (!haveLastSensorSeq) {
    extendedSensorSeq = sample.seq;
    haveLastSensorSeq = true;
  } else {
    // The one-byte packet id cannot reveal one or more complete 256-sample
    // wraps. For a large discontinuity, cadence and elapsed time can identify
    // a small restart/resync jump, but never make a long outage exact.
    const uint8_t raw_delta =
        static_cast<uint8_t>(sample.seq - lastSensorSeq);
    uint32_t delta = raw_delta;
    if (have_elapsed && nominal_cadence_ms != 0) {
      elapsedStepEstimate =
          (elapsed_ms + nominal_cadence_ms / 2U) /
          nominal_cadence_ms;
      if (elapsedStepEstimate == 0) {
        elapsedStepEstimate = 1;
      }
      if (raw_delta == 0) {
        // Same raw ID outside the duplicate window is a sensor restart/resync.
        // Use a small cadence-derived advance so the extended ID remains
        // monotonic; a complete wrap is inherently ambiguous.
        restartResync = true;
        if (elapsedStepEstimate >= 256U) {
          delta = 1;
          fullWrapAmbiguous = true;
        } else {
          delta = static_cast<uint32_t>(elapsedStepEstimate);
        }
      } else if (elapsedStepEstimate >= 256U) {
        // raw_delta is only the minimum modulo-256 advance. Keep monotonic
        // ordering while explicitly marking the missed count unknowable.
        restartResync = true;
        fullWrapAmbiguous = true;
      } else if (raw_delta > MAX_SMALL_RESYNC_STEPS &&
                 elapsedStepEstimate <= MAX_SMALL_RESYNC_STEPS) {
        delta = static_cast<uint32_t>(elapsedStepEstimate);
        restartResync = true;
      }
    } else if (raw_delta > MAX_SMALL_RESYNC_STEPS) {
      // With no cadence/elapsed evidence, advance monotonically once rather
      // than inventing the former fixed 20-second timing assumption.
      delta = 1;
      restartResync = true;
    }

    if (restartResync) {
      Serial.printf("[seq] sensor resync raw_prev=%u raw_now=%u "
                    "elapsed=%llu ms cadence=%lu ms elapsed_steps=%llu "
                    "applied_steps=%lu%s\n",
                    static_cast<unsigned int>(lastSensorSeq),
                    static_cast<unsigned int>(sample.seq),
                    static_cast<unsigned long long>(elapsed_ms),
                    static_cast<unsigned long>(nominal_cadence_ms),
                    static_cast<unsigned long long>(elapsedStepEstimate),
                    static_cast<unsigned long>(delta),
                    fullWrapAmbiguous ? " full-wrap-count-unknown" : "");
    }

    if (!sample.has_reported_interval && !restartResync &&
        have_elapsed) {
      observeLegacyCadence(elapsed_ms, raw_delta);
    }
    skipped = delta - 1U;
    extendedSensorSeq = static_cast<uint16_t>(extendedSensorSeq + delta);
  }

  const uint32_t advertised_interval_ms =
      sample.has_reported_interval ? sample.sample_interval_ms : 0;
  const bool status_metadata_changed =
      sensorMetadata.update(sample.configuration, advertised_interval_ms);

  lastSensorSeq = sample.seq;
  lastSensorSampleUptimeMs = clock.uptime_ms;
  haveLastSensorSampleUptime = true;
  StoredRecord record;
  record.time_s = clock.synced ? epochForUptime(clock.uptime_s, clock)
                               : clock.uptime_s;
  record.seq = extendedSensorSeq;
  record.temp_centi = sample.temp_centi;
  record.voltage_mv = sample.voltage_mv;
  record.lux_x1 = sample.lux_x1;
  record.hum_centi = sample.hum_centi;
  record.rssi = rssi;
  record.flags = clock.synced
                     ? static_cast<uint8_t>(RECORD_FLAG_CLOCK_SYNCED |
                                            RECORD_FLAG_EPOCH_VALID)
                     : 0;
  record.flags |= static_cast<uint8_t>(
      (sample.configuration << RECORD_FLAG_CONFIG_SHIFT) &
      RECORD_FLAG_CONFIG_MASK);
  if (sample.hat_layout) {
    record.flags |= RECORD_FLAG_TEMP_AMBIENT;
  }
  if (sample.hat_layout && sample.configuration == 4 &&
      sample.voltage_mv != INVALID_U16) {
    record.flags |= RECORD_FLAG_VSTOR_VALID;
  }

  recordStore.append(record, sample.seq);
  refreshLiveValue(true);
  refreshStatusValue(status_metadata_changed);

  const int32_t signed_temperature = sample.temp_centi;
  const uint32_t absolute_temperature = signed_temperature < 0
                                            ? static_cast<uint32_t>(-signed_temperature)
                                            : static_cast<uint32_t>(signed_temperature);
  Serial.printf(
      "[sample] sensor=%s raw_seq=%u ext_seq=%u skipped=%lu%s "
      "temp=%s%lu.%02lu C "
      "voltage=%u mV lux=%u hum=%u config=%u interval=%lu ms "
      "cadence_hint=%lu ms rssi=%d dBm uptime=%lu s synced=%u count=%lu\n",
      peer_address,
      static_cast<unsigned int>(sample.seq),
      static_cast<unsigned int>(extendedSensorSeq),
      static_cast<unsigned long>(skipped),
      restartResync ? " resync" : "",
      signed_temperature < 0 ? "-" : "",
      static_cast<unsigned long>(absolute_temperature / 100U),
      static_cast<unsigned long>(absolute_temperature % 100U),
      static_cast<unsigned int>(sample.voltage_mv),
      static_cast<unsigned int>(sample.lux_x1),
      static_cast<unsigned int>(sample.hum_centi),
      static_cast<unsigned int>(sample.configuration),
      static_cast<unsigned long>(advertised_interval_ms),
      static_cast<unsigned long>(
          sample.has_reported_interval
              ? sample.sample_interval_ms
              : learnedLegacyCadenceMs()),
      static_cast<int>(rssi),
      static_cast<unsigned long>(clock.uptime_s),
      static_cast<unsigned int>(clock.synced),
      static_cast<unsigned long>(recordStore.count()));
}

void connectCallback(uint16_t conn_handle) {
  peripheralConnHandle = conn_handle;

  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  char peer_name[32] = {0};
  if (connection != NULL) {
    connection->getPeerName(peer_name, sizeof(peer_name));
  }

  Serial.printf("[gatt] connected handle=%u peer=%s mtu=%u scan_running=%s\n",
                static_cast<unsigned int>(conn_handle),
                peer_name[0] != '\0' ? peer_name : "(unknown)",
                connection != NULL
                    ? static_cast<unsigned int>(connection->getMtu())
                    : 0U,
                Bluefruit.Scanner.isRunning() ? "yes" : "no");
}

void disconnectCallback(uint16_t conn_handle, uint8_t reason) {
  if (peripheralConnHandle == conn_handle) {
    peripheralConnHandle = BLE_CONN_HANDLE_INVALID;
  }
  historyRequests.discardConnection(conn_handle);
  liveNotifications.discardConnection(conn_handle);
  statusNotifications.discardConnection(conn_handle);
  if (eraseRequested && eraseRequestConnHandle == conn_handle) {
    eraseRequested = false;
    eraseRequestConnHandle = BLE_CONN_HANDLE_INVALID;
  }

  Serial.printf("[gatt] disconnected handle=%u reason=0x%02X; advertising "
                "will restart automatically\n",
                static_cast<unsigned int>(conn_handle),
                static_cast<unsigned int>(reason));
}

void cccdCallback(uint16_t conn_handle, BLECharacteristic* characteristic,
                  uint16_t cccd_value) {
  const char* name = "unknown";
  if (characteristic == &liveCharacteristic) {
    name = "LIVE";
  } else if (characteristic == &historyDataCharacteristic) {
    name = "HIST_DATA";
  } else if (characteristic == &statusCharacteristic) {
    name = "STATUS";
  }
  Serial.printf("[gatt] %s notifications %s on handle=%u\n", name,
                (cccd_value & BLE_GATT_HVX_NOTIFICATION) ? "enabled"
                                                         : "disabled",
                static_cast<unsigned int>(conn_handle));
}

void timeSyncWriteCallback(uint16_t conn_handle,
                           BLECharacteristic* characteristic,
                           uint8_t* data, uint16_t len) {
  (void)characteristic;
  if (len != 4) {
    Serial.printf("[timesync] rejected length=%u from handle=%u\n",
                  static_cast<unsigned int>(len),
                  static_cast<unsigned int>(conn_handle));
    return;
  }

  const uint32_t epoch = readLe32(data);
  const TimeSnapshot clock = bridgeClock.sync(epoch);
  recordStore.scheduleEpochPatches(clock);

  // A latest sample captured before this first sync should immediately become
  // readable with its corrected epoch, while retaining its stored flags bit.
  refreshLiveValue(false);
  refreshStatusValue(false);

  Serial.printf("[timesync] epoch=%lu uptime=%lu offset=%lld handle=%u\n",
                static_cast<unsigned long>(epoch),
                static_cast<unsigned long>(clock.uptime_s),
                static_cast<long long>(clock.epoch_offset),
                static_cast<unsigned int>(conn_handle));
}

void historyControlWriteCallback(uint16_t conn_handle,
                                 BLECharacteristic* characteristic,
                                 uint8_t* data, uint16_t len) {
  (void)characteristic;
  if (len == 5 && data[0] == HISTORY_REQUEST_OPCODE) {
    HistoryRequest request;
    request.conn_handle = conn_handle;
    request.since_epoch = readLe32(data + 1);
    const bool replaced = historyRequests.post(request);

    Serial.printf("[history] request since=%lu handle=%u%s\n",
                  static_cast<unsigned long>(request.since_epoch),
                  static_cast<unsigned int>(conn_handle),
                  replaced ? " (replaced pending request)" : "");
    return;
  }

  const bool valid_erase =
      len == 5 && data[0] == HISTORY_ERASE_ALL_OPCODE &&
      data[1] == 0x45 && data[2] == 0x52 &&
      data[3] == 0x41 && data[4] == 0x53;
  if (valid_erase) {
    if (!eraseRequested && !eraseInProgress) {
      // Defer all flash work to loop(); this callback only publishes a small,
      // volatile request shared with the Arduino task.
      eraseRequestConnHandle = conn_handle;
      eraseRequested = true;
      Serial.printf("[erase] request accepted handle=%u\n",
                    static_cast<unsigned int>(conn_handle));
    } else {
      Serial.printf("[erase] request ignored while erase is pending/active "
                    "handle=%u\n",
                    static_cast<unsigned int>(conn_handle));
    }
    return;
  }

  Serial.printf("[history] rejected control len=%u opcode=0x%02X handle=%u\n",
                static_cast<unsigned int>(len),
                static_cast<unsigned int>(len == 0 ? 0 : data[0]),
                static_cast<unsigned int>(conn_handle));
}

void statusReadCallback(uint16_t conn_handle,
                        BLECharacteristic* characteristic,
                        ble_gatts_evt_read_t* request) {
  (void)characteristic;

  const TimeSnapshot clock = bridgeClock.snapshot();
  uint8_t status[STATUS_SIZE];
  encodeStatus(clock, status);

  ble_gatts_rw_authorize_reply_params_t reply = {};
  reply.type = BLE_GATTS_AUTHORIZE_TYPE_READ;
  reply.params.read.gatt_status = BLE_GATT_STATUS_SUCCESS;

  if (request->offset <= STATUS_SIZE) {
    // Update the backing attribute atomically as part of authorizing this read.
    // SoftDevice then applies the central's requested offset to the new value.
    reply.params.read.update = 1;
    reply.params.read.offset = 0;
    reply.params.read.len = STATUS_SIZE;
    reply.params.read.p_data = status;
  } else {
    reply.params.read.gatt_status = BLE_GATT_STATUS_ATTERR_INVALID_OFFSET;
  }

  const uint32_t result =
      sd_ble_gatts_rw_authorize_reply(conn_handle, &reply);
  if (result != NRF_SUCCESS) {
    Serial.printf("[status] read authorization failed: 0x%08lX\n",
                  static_cast<unsigned long>(result));
  }
}

// ---------------------------------------------------------------------------
// Nonblocking notification services (run in Arduino loop task)
// ---------------------------------------------------------------------------

// Returns true only when a queued LIVE value still needs a TX slot.  History
// pauses in that case so it cannot repeatedly win the shared notification
// queue ahead of the real-time update.
static bool serviceLiveNotification() {
  PendingLiveNotification notification;
  if (!liveNotifications.peek(notification)) {
    return false;
  }

  if (!Bluefruit.connected(notification.conn_handle) ||
      !liveCharacteristic.notifyEnabled(notification.conn_handle)) {
    liveNotifications.complete(notification.generation);
    return false;
  }

  // notify() waits briefly for an HVN queue slot.  On queue pressure, retain
  // this exact generation and retry on the next loop; no LIVE update is lost.
  if (liveCharacteristic.notify(notification.conn_handle,
                                notification.record, RECORD_SIZE)) {
    liveNotifications.complete(notification.generation);
    return false;
  }
  return true;
}

// STATUS metadata changes share the same nonblocking retry discipline as LIVE.
// Failure to acquire a TX slot never delays scanning or flash persistence.
static bool serviceStatusNotification() {
  PendingStatusNotification notification;
  if (!statusNotifications.peek(notification)) {
    return false;
  }

  if (!Bluefruit.connected(notification.conn_handle) ||
      !statusCharacteristic.notifyEnabled(notification.conn_handle)) {
    statusNotifications.complete(notification.generation);
    return false;
  }

  if (statusCharacteristic.notify(notification.conn_handle,
                                  notification.status, STATUS_SIZE)) {
    statusNotifications.complete(notification.generation);
    return false;
  }
  return true;
}

static void abortHistory(const char* reason) {
  Serial.printf("[history] aborted after %lu/%lu records: %s\n",
                static_cast<unsigned long>(historyStream.next_record),
                static_cast<unsigned long>(historyStream.record_count), reason);
  historyStream.active = false;
}

static void startHistory(const HistoryRequest& request) {
  if (!Bluefruit.connected(request.conn_handle)) {
    Serial.println("[history] discarded request from disconnected client");
    return;
  }
  if (!historyDataCharacteristic.notifyEnabled(request.conn_handle)) {
    Serial.println("[history] HIST_DATA notifications are not enabled");
    return;
  }

  historyStream.conn_handle = request.conn_handle;
  historyStream.since_epoch = request.since_epoch;
  historyStream.clock = bridgeClock.snapshot();
  historyStream.record_count = recordStore.iterateSince(
      request.since_epoch, historyStream.clock, historySnapshot,
      RECORD_CAPACITY);
  historyStream.next_record = 0;
  historyStream.last_progress_log = 0;
  historyStream.active = true;

  BLEConnection* connection = Bluefruit.Connection(request.conn_handle);
  const uint16_t mtu = connection != NULL ? connection->getMtu() : 0;
  Serial.printf("[history] streaming %lu record(s), since=%lu, mtu=%u\n",
                static_cast<unsigned long>(historyStream.record_count),
                static_cast<unsigned long>(request.since_epoch),
                static_cast<unsigned int>(mtu));
}

static void serviceHistoryStream() {
  if (!historyStream.active) {
    HistoryRequest request;
    if (historyRequests.take(request)) {
      startHistory(request);
    }
    return;
  }

  if (!Bluefruit.connected(historyStream.conn_handle)) {
    abortHistory("client disconnected");
    return;
  }
  if (!historyDataCharacteristic.notifyEnabled(historyStream.conn_handle)) {
    abortHistory("HIST_DATA notifications disabled");
    return;
  }

  BLEConnection* connection =
      Bluefruit.Connection(historyStream.conn_handle);
  if (connection == NULL) {
    abortHistory("connection object unavailable");
    return;
  }

  if (historyStream.next_record < historyStream.record_count) {
    const uint16_t mtu = connection->getMtu();
    size_t records_per_packet = 1;
    if (mtu > 4) {
      records_per_packet = (mtu - 3U - 1U) / RECORD_SIZE;
      if (records_per_packet == 0) {
        records_per_packet = 1;
      }
    }
    if (records_per_packet > MAX_RECORDS_PER_NOTIFICATION) {
      records_per_packet = MAX_RECORDS_PER_NOTIFICATION;
    }

    const size_t remaining =
        historyStream.record_count - historyStream.next_record;
    const size_t packet_records = remaining < records_per_packet
                                      ? remaining
                                      : records_per_packet;

    uint8_t packet[MAX_HISTORY_NOTIFICATION_SIZE];
    packet[0] = static_cast<uint8_t>(packet_records);
    for (size_t i = 0; i < packet_records; ++i) {
      encodeRecord(historySnapshot[historyStream.next_record + i],
                   historyStream.clock, packet + 1 + i * RECORD_SIZE);
    }

    const uint16_t packet_size = static_cast<uint16_t>(
        1 + packet_records * RECORD_SIZE);

    // Never advance next_record unless this exact notification was accepted by
    // the SoftDevice.  A full HVN queue therefore causes a retry, not a gap.
    if (!historyDataCharacteristic.notify(historyStream.conn_handle, packet,
                                          packet_size)) {
      return;
    }

    historyStream.next_record += packet_records;
    if (historyStream.next_record == historyStream.record_count ||
        historyStream.next_record - historyStream.last_progress_log >= 64) {
      historyStream.last_progress_log = historyStream.next_record;
      Serial.printf("[history] progress %lu/%lu records\n",
                    static_cast<unsigned long>(historyStream.next_record),
                    static_cast<unsigned long>(historyStream.record_count));
    }
    return;
  }

  const uint8_t end_marker = 0xFF;
  if (!historyDataCharacteristic.notify(historyStream.conn_handle, &end_marker,
                                        sizeof(end_marker))) {
    return;  // Retry the marker too; completion is never inferred locally.
  }

  Serial.printf("[history] complete: %lu record(s), end marker sent\n",
                static_cast<unsigned long>(historyStream.record_count));
  historyStream.active = false;
}

static bool serviceEraseRequest() {
  if (!eraseRequested) {
    return false;
  }

  // An in-flight history transfer has a real wire-level completion marker.
  // Send it before invalidating the stable snapshot so the client never waits
  // for a stream that can no longer finish.
  if (historyStream.active) {
    if (Bluefruit.connected(historyStream.conn_handle) &&
        historyDataCharacteristic.notifyEnabled(historyStream.conn_handle)) {
      const uint8_t end_marker = 0xFF;
      if (!historyDataCharacteristic.notify(historyStream.conn_handle,
                                            &end_marker,
                                            sizeof(end_marker))) {
        return true;  // TX queue full; retry before erasing anything.
      }
      Serial.println("[erase] active history stream ended before erase");
    }
    historyStream.active = false;
  }

  const uint16_t request_conn_handle = eraseRequestConnHandle;
  eraseRequested = false;
  eraseInProgress = true;
  eraseRequestConnHandle = BLE_CONN_HANDLE_INVALID;
  historyRequests.clear();
  liveNotifications.discardConnection(request_conn_handle);
  statusNotifications.discardConnection(request_conn_handle);

  const bool erased = recordStore.eraseAll();
  if (erased) {
    // Preserve bridgeClock and sensor metadata. Only retained sample state is
    // reset; LIVE becomes its defined empty value and STATUS reports count 0.
    uint8_t empty_record[RECORD_SIZE] = {0};
    liveCharacteristic.write(empty_record, sizeof(empty_record));
    refreshStatusValue(true);
  }
  eraseInProgress = false;
  return false;
}

// ---------------------------------------------------------------------------
// GATT, advertising, scanning, and Arduino entry points
// ---------------------------------------------------------------------------

static void fatalSetup(const char* message) {
  Serial.printf("[fatal] %s\n", message);
  while (true) {
    delay(1000);
  }
}

static void setupGatt() {
  if (bridgeService.begin() != ERROR_NONE) {
    fatalSetup("failed to begin bridge service");
  }

  liveCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  liveCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  liveCharacteristic.setFixedLen(RECORD_SIZE);
  liveCharacteristic.setCccdWriteCallback(cccdCallback);
  if (liveCharacteristic.begin() != ERROR_NONE) {
    fatalSetup("failed to begin LIVE characteristic");
  }
  uint8_t empty_record[RECORD_SIZE] = {0};
  liveCharacteristic.write(empty_record, sizeof(empty_record));

  historyControlCharacteristic.setProperties(CHR_PROPS_WRITE);
  historyControlCharacteristic.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  historyControlCharacteristic.setFixedLen(5);
  historyControlCharacteristic.setWriteCallback(historyControlWriteCallback);
  if (historyControlCharacteristic.begin() != ERROR_NONE) {
    fatalSetup("failed to begin HIST_CTRL characteristic");
  }

  historyDataCharacteristic.setProperties(CHR_PROPS_NOTIFY);
  // Bluefruit derives CCCD write permission from the value's read permission,
  // so OPEN is required here even though the characteristic has no Read prop.
  historyDataCharacteristic.setPermission(SECMODE_OPEN,
                                          SECMODE_NO_ACCESS);
  historyDataCharacteristic.setMaxLen(MAX_HISTORY_NOTIFICATION_SIZE);
  historyDataCharacteristic.setCccdWriteCallback(cccdCallback);
  if (historyDataCharacteristic.begin() != ERROR_NONE) {
    fatalSetup("failed to begin HIST_DATA characteristic");
  }

  timeSyncCharacteristic.setProperties(CHR_PROPS_WRITE);
  timeSyncCharacteristic.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  timeSyncCharacteristic.setFixedLen(4);
  timeSyncCharacteristic.setWriteCallback(timeSyncWriteCallback);
  if (timeSyncCharacteristic.begin() != ERROR_NONE) {
    fatalSetup("failed to begin TIMESYNC characteristic");
  }

  statusCharacteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_NOTIFY);
  statusCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  statusCharacteristic.setFixedLen(STATUS_SIZE);
  statusCharacteristic.setReadAuthorizeCallback(statusReadCallback);
  statusCharacteristic.setCccdWriteCallback(cccdCallback);
  if (statusCharacteristic.begin() != ERROR_NONE) {
    fatalSetup("failed to begin STATUS characteristic");
  }
  uint8_t initial_status[STATUS_SIZE] = {0};
  initial_status[10] = FW_VERSION;
  statusCharacteristic.write(initial_status, sizeof(initial_status));
}

static void startPeripheralAdvertising() {
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.setType(
      BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED);

  if (!Bluefruit.Advertising.addFlags(
          BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE) ||
      !Bluefruit.Advertising.addService(bridgeService) ||
      !Bluefruit.ScanResponse.addName()) {
    fatalSetup("failed to build advertising data");
  }

  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(ADV_FAST_INTERVAL_UNITS,
                                    ADV_SLOW_INTERVAL_UNITS);
  Bluefruit.Advertising.setFastTimeout(30);
  if (!Bluefruit.Advertising.start(0)) {
    fatalSetup("failed to start connectable advertising");
  }
}

static void startObserverScanning() {
  Bluefruit.Scanner.setRxCallback(scanCallback);
  Bluefruit.Scanner.restartOnDisconnect(true);
  Bluefruit.Scanner.useActiveScan(false);
  Bluefruit.Scanner.setInterval(SCAN_INTERVAL_UNITS, SCAN_WINDOW_UNITS);
  if (!Bluefruit.Scanner.start(0)) {
    fatalSetup("failed to start passive scanning");
  }
}

void setup() {
  Serial.begin(115200);
  // Do not wait for USB CDC: logging is always enabled, but bridge operation
  // must begin even when no serial monitor is attached.
  delay(50);

  Serial.println();
  Serial.println("HSC T+ Bridge firmware v3 starting");
  if (SENSOR_ADDRESS_FILTER_ENABLED) {
    Serial.printf("[commissioning] sensor allowlist=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  SENSOR_ADDRESS[0], SENSOR_ADDRESS[1], SENSOR_ADDRESS[2],
                  SENSOR_ADDRESS[3], SENSOR_ADDRESS[4], SENSOR_ADDRESS[5]);
  } else {
    Serial.println("[commissioning] WARNING: sensor address filter is disabled");
    Serial.println("[commissioning] use this build only to discover an address");
  }

  if (!bridgeClock.begin() || !recordStore.begin() ||
      !historyRequests.begin() || !liveNotifications.begin() ||
      !statusNotifications.begin() || !sensorMetadata.begin()) {
    fatalSetup("failed to allocate a synchronization primitive");
  }
  uint8_t restored_raw = 0; uint16_t restored_ext = 0;
  if (recordStore.lastRaw(restored_raw, restored_ext)) {
    haveLastSensorSeq = true; lastSensorSeq = restored_raw; extendedSensorSeq = restored_ext;
    Serial.printf("[storage] restored %lu records raw_seq=%u ext_seq=%u\n", static_cast<unsigned long>(recordStore.count()), restored_raw, restored_ext);
  }

  // Keep the 247-byte ATT MTU and three-packet notification queue, but use a
  // short peripheral event so the observer retains scanner airtime.
  Bluefruit.configPrphConn(MAX_ATT_MTU, 6, 3, 1);

  // One peripheral link for the phone plus one central-role allocation for the
  // observer/scanner.  The bridge never initiates a central connection.
  if (!Bluefruit.begin(1, 1)) {
    fatalSetup("Bluefruit.begin(1, 1) failed");
  }
  Bluefruit.setName(DEVICE_NAME);
  Bluefruit.setTxPower(0);
  Bluefruit.autoConnLed(false);

  Bluefruit.Periph.setConnectCallback(connectCallback);
  Bluefruit.Periph.setDisconnectCallback(disconnectCallback);

  setupGatt();
  startPeripheralAdvertising();
  startObserverScanning();

  Serial.printf("[ready] advertising as %s; passive scan interval/window="
                "%u/%u (100%% duty)\n",
                DEVICE_NAME, static_cast<unsigned int>(SCAN_INTERVAL_UNITS),
                static_cast<unsigned int>(SCAN_WINDOW_UNITS));
}

void loop() {
  const bool erase_waiting_for_tx = serviceEraseRequest();
  if (!erase_waiting_for_tx) {
    recordStore.service();
  }
  if (scanResumePending && Bluefruit.Scanner.resume()) {
    scanResumePending = false;
    Serial.println("[scan] scanner resumed");
  }

  // LIVE gets first use of an available TX queue slot, STATUS metadata changes
  // get second use, and history then advances by at most one notification.
  // BLE callbacks run in the core's separate Ada callback task, so scanning
  // continues during even a full history transfer.
  if (!erase_waiting_for_tx && !serviceLiveNotification()) {
    if (!serviceStatusNotification()) {
      serviceHistoryStream();
    }
  }

  // Refresh the extended uptime frequently enough that no raw tick-counter
  // rollover can ever pass unnoticed, even during months unattended.
  static TickType_t last_clock_maintenance_tick = 0;
  const TickType_t now = xTaskGetTickCount();
  if (static_cast<TickType_t>(now - last_clock_maintenance_tick) >=
      configTICK_RATE_HZ) {
    (void)bridgeClock.snapshot();
    last_clock_maintenance_tick = now;
  }

  delay(2);
}
