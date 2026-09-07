# HSC T+ frozen wire protocol contract — version 3

Sensor advertisement revision: **3.1**. This contract is shared with the
separate [HSC T+ web app](https://github.com/brianc-5/HSC-IoT_Temp).

All multi-byte values are little-endian. The sensor is a Seeed Studio XIAO
nRF52840 running Zephyr; the USB-powered bridge is a second XIAO nRF52840 using
the non-mbed Seeed nRF52/Bluefruit core.

Protocol v3 keeps older bare-board sensor advertisements readable while adding
automatic hardware configuration reporting, an advertised sample cadence,
Logger HAT measurements, and storage-power analytics.

Sensor advertisement revision 3.1 moves cadence from standard BTHome duration
object `0x42` into a length-prefixed raw `0x54` object. The new bridge accepts
both encodings, so earlier enhanced/HAT v3 sensors and the original 11-byte
legacy advertisement remain readable. Older bridges do not recognize the new
required raw cadence, so update the sensor and bridge together. GATT
firmware/status version stays `3` because the GATT record schema is unchanged.

## Sensor configurations

The same sensor firmware detects its hardware and power source automatically.

| ID | Configuration | Measurements | Power analytics |
| ---: | --- | --- | --- |
| 0 | Legacy/unknown | Determined from fields | Disabled unless a record explicitly marks VSTOR valid |
| 1 | Bare XIAO + USB | MCU die temperature, regulated VDD | Disabled |
| 2 | Bare XIAO + external power | MCU die temperature, regulated VDD | Disabled: VSTOR is not observable without a divider |
| 3 | XIAO + Logger HAT + USB | Ambient temperature, humidity, lux | Disabled while USB-powered |
| 4 | XIAO + Logger HAT + external power | Ambient temperature, humidity, lux, VSTOR when valid | Enabled for records with valid VSTOR |

The firmware probes the SHT40 and BH1750 only while the HAT rail is enabled.
It detects USB VBUS through the nRF52840 POWER peripheral without enabling the
USB stack.

## Sensor advertisement

The sensor emits a legacy, non-connectable, non-scannable, undirected BTHome v2
advertisement with Flags `02 01 06` and Service Data UUID `0xFCD2`. The BTHome
device-information byte is `0x40` (version 2, unencrypted, regular updates).

Objects are in ascending ID order.

| Object | Encoding | Meaning |
| --- | --- | --- |
| `0x00` | uint8 | Packet ID, incremented once per sample |
| `0x02` | sint16, 0.01 degC | Ambient SHT40 or MCU die temperature |
| `0x03` | uint16, 0.01 %RH | SHT40 humidity |
| `0x05` | uint24, 0.01 lux | BH1750 illuminance |
| `0x0C` | uint16, 0.001 V | Regulated VDD in bare mode or VSTOR in full HAT mode |
| `0x42` | uint24, 0.001 s | Earlier-v3 configured interval; accepted for compatibility |
| `0x54` | raw: uint8 length `03`, then uint24 milliseconds | Configured interval between new samples |
| `0xF0` | uint16 | Configuration ID from the table above |

### Enhanced bare form

The service-data array passed to `BT_DATA_SVC_DATA16`, including UUID bytes, is
19 bytes:

```text
D2 FC 40 00 ss 02 tt tt 0C vv vv 54 03 ii ii ii F0 cc cc
```

The complete advertising data is 24 bytes including Flags and AD headers.

### Logger HAT form

The service-data array is 26 bytes:

```text
D2 FC
40
00 ss
02 tt tt
03 hh hh
05 ll ll ll
0C vv vv
54 03 ii ii ii
F0 cc cc
```

The complete advertising data is exactly the 31-byte legacy limit.

### Legacy form

Older sensor firmware emits the existing 11-byte service-data array:

```text
D2 FC 40 00 ss 02 tt tt 0C vv vv
```

The bridge accepts it as configuration 0. Its voltage is VDD, never VSTOR.
The bridge walks objects rather than relying on offsets: unrelated standard
BTHome objects with a known encoded size are skipped safely, while duplicate
target objects, truncated values, and unknown-size IDs reject the frame.

### Invalid sentinels

| Field | Sentinel |
| --- | --- |
| Temperature | `0x8000` (`INT16_MIN`) |
| Humidity | `0xFFFF` |
| Illuminance on air | `0xFFFFFF` |
| Voltage | `0xFFFF` |
| Stored integer lux | `0xFFFF`; valid values saturate at 65534 |

An unavailable or failed field uses its sentinel. One failed sensor never
suppresses the rest of the advertisement.

## Bridge GATT service

Service UUID: `8f0e0001-9d7d-4d5a-9f2b-3c4a5b6c7d8e`

| Characteristic | UUID | Properties | Value |
| --- | --- | --- | --- |
| LIVE | `8f0e0002-...` | Read, Notify | One 16-byte Record v3 |
| HIST_CTRL | `8f0e0003-...` | Write | History request or guarded erase command |
| HIST_DATA | `8f0e0004-...` | Notify | uint8 count + Records; `FF` ends transfer |
| TIMESYNC | `8f0e0005-...` | Write | uint32 Unix epoch seconds |
| STATUS | `8f0e0006-...` | Read, Notify | 16-byte Status v3 |

HIST_DATA packs:

```text
floor((negotiated ATT_MTU - 3 - 1) / 16)
```

complete records in each notification. The end marker is the single byte
`FF`, not a count-prefixed packet. `since_epoch = 0` requests every retained
record. Records without a known epoch are always included.

HIST_CTRL accepts exactly two five-byte commands:

```text
01 ee ee ee ee    request records since little-endian Unix epoch
02 45 52 41 53    erase all history (`02` + ASCII `ERAS`)
```

ERASE_ALL is deferred out of the GATT callback. If a history stream is active,
the bridge sends its `FF` end marker first, cancels pending history/epoch-patch
work, and erases all 64 primary plus 64 patch sectors while yielding between
sectors. Samples arriving during that physical erase are deliberately dropped.
On success all record-store RAM state is reset, LIVE becomes its empty value,
and STATUS notifies with record count zero. The bridge clock synchronization
and current sensor metadata are preserved.

## Record v3

Each LIVE/HIST_DATA record is exactly 16 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | uint32 | Unix epoch seconds, or zero when unknown |
| 4 | uint16 | Bridge-maintained extended sequence |
| 6 | sint16 | Temperature in 0.01 degC |
| 8 | uint16 | Voltage in mV; VDD or VSTOR according to flags |
| 10 | uint16 | Integer lux; `0xFFFF` invalid |
| 12 | uint16 | Humidity in 0.01 %RH; `0xFFFF` invalid |
| 14 | int8 | RSSI observed by the bridge |
| 15 | uint8 | Record flags |

Record flag bits:

| Bits | Meaning |
| --- | --- |
| 0 | Bridge clock was synchronized at capture |
| 1..3 | Configuration ID 0..7 (currently 0..4) |
| 4 | Voltage is valid VSTOR and may enter storage-power calculations |
| 5 | Temperature is ambient SHT40 rather than MCU die temperature |
| 6..7 | Always zero on the wire; reserved for bridge-internal time state |

When bit 4 is clear, voltage may be diagnostic VDD or invalid. It must not
enter storage-power calculations.

## Status v3

STATUS is exactly 16 bytes:

| Offset | Type | Meaning |
| ---: | --- | --- |
| 0 | uint32 | Retained record count |
| 4 | uint32 | Last sample age in seconds; `0xFFFFFFFF` means unknown |
| 8 | int8 | Last sensor RSSI |
| 9 | uint8 | Status flags; bit 0 means bridge clock synchronized |
| 10 | uint8 | Bridge firmware version, exactly `3` |
| 11 | uint8 | Current sensor configuration ID |
| 12 | uint32 | Current advertised sample interval in milliseconds; zero unknown |

STATUS notifies when the current configuration or reported cadence changes.
The webapp reads and validates STATUS before TIMESYNC or any Record parsing.

## Cadence independence

The bridge does not require a fixed sensor interval. It uses the three-byte
payload of raw object `0x54` when present, accepts the standard three-byte
duration object `0x42` from earlier v3 sensors, and learns a robust median
cadence for legacy advertisements. Raw lengths other than three are rejected.
The standard one-byte UV-index object is `0x46` and is safely skipped.

Cadence is a hint for sequence restart/resynchronization and duplicate
classification. A repeated uint8 packet ID within 1.5 nominal sample periods
is a burst duplicate. The same ID later than that is accepted through the
restart/resync path, so a sensor reboot cannot suppress new samples forever.
When neither reported nor learned cadence exists, the bridge preserves the
packet because it lacks evidence that it is a duplicate.

A uint8 packet ID cannot reveal an exact missed-sample count after one or more
complete 256-sample wraps. In that case the bridge preserves monotonic record
ordering and reports a resynchronization without inventing an exact count.

## Persistent flash compatibility

Every base flash slot remains exactly 32 bytes and retains the old offsets:

```text
0..3    magic
4..7    generation
8..19   legacy 12-byte stored core
20      raw packet ID
21..29  extension
30..31  CRC-16
```

Extension layout:

```text
0..1  integer lux
2..3  humidity centi-percent
4     disk-format marker (`3` for Protocol v3)
5..8  reserved zero
```

Marker zero identifies legacy zero-initialized slots. Their CRC, time,
sequence, die temperature, VDD, RSSI, and packet ID remain valid. Marker 3
slots add the environmental fields and capability flags. Epoch patches,
generation ordering, torn-write rejection, and CRC coverage are unchanged.

## Time model

Before synchronization the bridge stores current-boot uptime. TIMESYNC sets:

```text
epoch_offset = written_epoch - uptime_at_write
```

Records captured after synchronization store epoch immediately. Current-boot
pre-sync records are normalized and written to the separate CRC-checked
epoch-patch log. Unpatched records recovered from a prior boot have epoch zero
instead of being interpreted using a later boot's offset.

## Storage-power model

The canonical plausible VSTOR input band is **1500 through 4500 mV,
inclusive**. The sensor build defaults and bridge validation use these same
limits. Values outside the band remain available only as invalid sentinels and
cannot enter analytics.

Only records whose flag bit 4 marks valid VSTOR participate:

```text
P_net = C * VSTOR * dVSTOR/dt
```

The webapp estimates dark drain from stable low-lux bands and then estimates
harvest delivered into storage:

```text
P_harvest_to_storage = max(0, P_net + P_dark_drain)
```

It evaluates cumulative bands below 1, 3, and 5 lux. A candidate needs at least
30 net-power estimates spanning a full regression window. The lowest adjacent
pair that differs by no more than `max(5 uW, 20%)` is preferred; a non-positive
drain or materially inconsistent bands leave harvest unavailable rather than
substituting a zero baseline.

This is power delivered into VSTOR, not raw panel-terminal power and not
post-buck load power.
