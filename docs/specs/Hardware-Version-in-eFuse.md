# Hardware Version in eFuse

## Status

**Implemented.** Applies to MK11 (Carrot / ESP32-C6) onward. Earlier generations
(MK5–MK10) are never burned and are read as "unknown hardware version" — they
keep relying on [MAC-based device selection](MAC-Based-Device-Selection.md).

## Motivation

Device selection currently relies on matching a prefix of the WiFi MAC address
to a `DeviceDefinition` subclass (see
[MAC-Based-Device-Selection.md](MAC-Based-Device-Selection.md)). This worked
while each hardware generation used a distinct MAC block, but the MK11 batch
shipped with MAC addresses that are not reliably distinguishable from MK10's.
MAC-based dispatch is a heuristic built on an assumption (stable per-generation
MAC blocks) that no longer holds, so identification needs to move to a value
that is authoritative by construction: a record burned into the chip itself
during board test.

`startDeviceBasedOnHardware()` in `main/main.cpp` now checks the eFuse
identity first and only falls back to MAC prefix matching when it's absent.
See "Device selection" below.

## Design

### One eFuse block, burned once

The ESP32-S3 and ESP32-C6 apply Reed-Solomon (RS) coding automatically to
every eFuse block above `BLOCK0` — 12 check-symbol bytes computed over the
full 256-bit block. This has a consequence that isn't obvious up front: **an
RS-coded block can only be meaningfully burned once.** Writing anything into
an already-burned block — even previously-untouched bits — recomputes the
checksum over data that doesn't match what's physically there and corrupts
the whole block. This was verified directly against `espefuse` (`v5.3.1`,
bundled with IDF 6.0.2): burning a second, different payload into an
already-burned `BLOCK_USR_DATA` fails outright (`Burn into BLOCK_USR_DATA is
forbidden (RS coding scheme does not allow this)`) unless the caller passes
`--force-write-always`, which does not undo the checksum corruption — it just
skips the tool's own guardrail.

That's a fine constraint for this record: every field (`hw_gen`, `hw_rev`,
`mfr_id`, `batch`, `serial`) is known and fixed at post-assembly board test
time, so one atomic burn is all that's needed. The record lives in `BLOCK3`, which
ESP-IDF already exposes through its common eFuse table — no custom eFuse
table CSV or codegen step is needed:

| Purpose         | eFuse block | ESP-IDF symbol        | espefuse block name            |
| --------------- | ----------- | --------------------- | ------------------------------ |
| Identity record | `BLOCK3`    | `ESP_EFUSE_USER_DATA` | `BLOCK_USR_DATA` / `USER_DATA` |

### Record layout

```c
typedef struct __attribute__((packed)) {
    uint16_t magic;       // 0x5544 ('UD') — must match before trusting any other field
    uint16_t fmt_version; // 0x0001
    uint16_t hw_gen;      // hardware generation (MK number, e.g. 11 for MK11)
    uint16_t hw_rev;      // hardware sub-revision (1 = first release of that generation)
    uint16_t mfr_id;      // manufacturer / assembler ID (0x0000 = unknown)
    uint64_t batch;       // manufacturer batch/lot ID; 0 = not recorded
    uint64_t serial;      // unit serial number
} ud_hw_identity_t;       // 26 bytes, burned into BLOCK3 (USER_DATA), zero-padded to the full 32-byte block
```

The record only uses 26 of the block's 32 bytes; the remaining 6 bytes are
zero-padded and unused. Every field beyond `magic` was sized up to the
widest convenient width that still fits — since an RS-coded block can only be
burned once (see below), there's no way to widen a field later, so headroom
is cheap to buy now and expensive to add after the fact.

`mfr_id` values are assigned in a registry as manufacturers come online;
`0x0000` means unknown/unburned.

| `mfr_id` | Manufacturer       |
| -------- | ------------------ |
| `0x0000` | Unknown / unburned |
| `0x0001` | JLCPCB             |

#### `batch`

JLCPCB prints a batch/lot code on assembly labels and QR codes (e.g.
`70kbl`) — base-36 over `[0-9a-z]`. A 5-character code has up to
36⁵ ≈ 60.5M possible values (≈ 25.85 bits), which fits comfortably in a
64-bit field with plenty of room for longer codes from other manufacturers.
`batch` stores this as a plain integer (the numeric value of the base-36
string), not the string itself; `tools/efuse_burn.py show` prints it as hex.
Optional: not every manufacturer produces a code like this, so `0` means
"not recorded", same convention as `mfr_id`.

There's no standard prefix for base-36 the way `0x` means hex, so
`tools/efuse_burn.py` uses `0z` by analogy (e.g. `--batch 0z70kbl`) — handled
by the tool's shared `parse_int()` helper, so it also works for `--mfr-id`
and `--serial` if that's ever useful, not just `--batch`. A bare code without
the `0z` prefix is rejected rather than silently guessed, since e.g. `abc12`
would otherwise be ambiguous between hex and base-36.

### Endianness

The record's byte layout is **native little-endian**, with no per-field
exceptions:

- ESP-IDF's own eFuse documentation states the bit/byte order of eFuse blocks
  is little-endian (bytes are read/written LSB→MSB;
  `esp_efuse_read_field_blob()`/`esp_efuse_write_field_blob()` are effectively
  a `memcpy` against that layout).
- Both target CPUs (Xtensa on ESP32-S3, RISC-V on ESP32-C6) run little-endian
  in this project's ESP-IDF configuration.

Since the hardware, the CPU, and the read/write API are all natively
little-endian, there's no reason to special-case any field's byte order — a
per-field exception would only add a byte-swap step that has to be kept in
sync between firmware and the burn tool for no benefit.

### Integrity: no application-level CRC

eFuse bits are physically one-way, which raises the question of how a burned
record is protected from corruption or partial re-burns. It turns out an
application-level checksum isn't needed here:

- RS coding already gives hardware-level error detection _and_ correction
  (up to 6 bad bytes per 32-byte block). A read that hits an uncorrectable
  error returns `ESP_ERR_DAMAGED_READING` from `esp_efuse_read_field_blob()`
  — confirmed in `esp_efuse_utility.c` — and the API already retries
  transiently-damaged reads internally.
- `espefuse` itself refuses a second, differing burn into an already-burned
  RS-coded block by default (verified above) — the "single-shot" boundary is
  enforced at burn time, not something a reader has to detect after the fact.
- The `magic`/`fmt_version` fields already distinguish "never burned" (reads
  as all-zero) from "burned and valid", which is all that's needed to decide
  whether the rest of the record can be trusted.

Adding a CRC on top would duplicate protection the platform already provides
without adding a real capability, so this design skips it.

## Firmware API

`components/kernel/src/HardwareVersion.hpp` reads the record lazily (mirrors
the caching pattern in `MacAddress.hpp`) and exposes:

```cpp
struct HardwareVersion {
    uint16_t hwGen;
    uint16_t hwRev;
    uint16_t mfrId;
    uint64_t batch;
    uint64_t serial;
};

// std::nullopt if the block is unburned, unreadable, or fails its magic/format check.
const std::optional<HardwareVersion>& getHardwareVersion();
```

## Device selection

`startDeviceBasedOnHardware()` in `main/main.cpp` calls `getHardwareVersion()`
once, at the top of the function, before any device-selection logic. It logs
the result there (or an informational message for unburned/legacy boards) and
reuses the same value for dispatch below — this is the only place in firmware
that reads the eFuse record.

For the ESP32-C6 (Carrot) runtime-detection path, dispatch checks the eFuse
identity before falling back to MAC prefix matching:

1. If `getHardwareVersion()` returns a value, match `(hwGen, hwRev)` against
   the known `(hw_gen, hw_rev)` pairs for each `DeviceDefinition` subclass and
   instantiate directly. Note `hw_rev` is 1-indexed (`1` = first release of
   the generation), so e.g. `UglyDucklingMk11Rev1` corresponds to
   `hw_gen == 11 && hw_rev == 1`.
2. If the eFuse record is present but doesn't match any known pair, log a
   warning and fall through to step 3 rather than aborting — an unrecognized
   but validly-burned record is more likely a firmware/hardware version skew
   than something to fail hard on.
3. If there's no eFuse record at all (unburned — every MK10 in the field
   today, and any MK11 built before board test started burning eFuse), fall
   back to the existing MAC prefix chain.

This is the fix for the motivating problem: once board test burns the
identity record, device selection no longer depends on MAC address ranges
staying distinct across generations. The ESP32-S3 (Spinach) path is
unchanged — those generations predate this scheme and won't be retroactively
burned, so there's nothing for an eFuse check to match there.

## Burn tooling

`tools/efuse_burn.py` wraps `espefuse` for board test. It shells out rather
than re-implementing eFuse programming, matching how `scripts/gen_config_nvs.py`
shells out to `esptool`'s NVS partition generator.

```sh
# Post-assembly board test — one-time identity burn
tools/efuse_burn.py identity --port /dev/ttyUSB0 \
    --hw-gen 11 --hw-rev 1 --mfr-id 1 --batch 0z70kbl --serial 1042

# Equivalent, scanning JLCPCB's assembly label QR code instead
tools/efuse_burn.py identity --port /dev/ttyUSB0 --jlcpcb-qr UD11R01_70kbl_1042

# Read back and decode the record for verification
tools/efuse_burn.py show --port /dev/ttyUSB0
```

`--chip` is optional — if omitted, `espefuse` auto-detects it from whatever
board is connected, same as running `espefuse` directly. `--port` has no such
auto-detection in `espefuse` and is always required (unless `--virt` is set,
where it's meaningless anyway) — omitting it fails fast with a clear error
rather than silently guessing a port.

Serial numbers are supplied by the caller (`--serial`); this tool has no
opinion on how they're issued or tracked — that's board-test process, not
firmware.

### `--jlcpcb-qr`

JLCPCB's assembly labels print a QR code of the form `UD11R01_70kbl_0005` —
generation, `R` + revision, batch/lot code, and serial, underscore-separated.
`--jlcpcb-qr` parses all four fields out of that string in one shot, instead
of requiring the board-test operator to key in `--hw-gen`/`--hw-rev`/`--batch`/
`--serial` individually (and risk a transcription error). It's mutually
exclusive with those four flags — combine `--jlcpcb-qr` with either all of
them or none of them, not some.

Neither the batch nor the serial segment is assumed to be a fixed width (e.g.
`70kbl` vs a single-character code, or `0005` vs `000123`), since neither
JLCPCB's own codes nor future manufacturers' are guaranteed to match the
5-character example above.

Since the QR format is JLCPCB-specific, `--jlcpcb-qr` also defaults `--mfr-id`
to `0x0001` (JLCPCB) — pass `--mfr-id` explicitly alongside it to override
that (e.g. a different assembler relabeling JLCPCB-made boards).

Add `--virt --chip {esp32s3,esp32c6} --path-efuse-file <file>` to any
subcommand to dry-run against a virtual eFuse file with no hardware attached
(useful for testing the tool itself; this is also how the RS single-shot
behavior above was verified). `--chip` must be given explicitly in this
mode — there's no board to auto-detect it from.

`tools/test/test_efuse_burn.py` exercises the tool end-to-end against `--virt`
(round-tripping a burn through `show`, the unburned/no-record case, the
rejected-second-burn case, and CLI argument validation). Run it locally with
`python3 -m unittest discover -s tools/test -v` after sourcing
`tools/activate_idf.sh`; CI runs it as a step in the `embedded-test` job
(chosen to avoid spinning up a separate job/runner just for this) via
`espressif/esp-idf-ci-action`, the same ESP-IDF Docker image the `build` job
uses — so `espefuse` is exactly the version bundled with `ESP_IDF_VERSION`,
with no separate Python/esptool version to pin or keep in sync.

## Deferred: mutable flags (e.g. RMA tracking)

A natural extension is a `flags` field for state that changes after the
initial burn — e.g. an `FLAG_RMA` bit set when a unit is returned and
reworked. That's left out of this iteration: it's not needed yet, and it
isn't free to add, because a block can only be burned once (see above) — a
flag that needs to change _after_ the factory burn cannot live in the same
block as the identity record without permanently locking out every future
flag update the first time the identity block is burned.

If/when this is needed, the natural design is a **second, independently
RS-coded eFuse block** dedicated to flags — e.g. `BLOCK4` (`ESP_EFUSE_KEY0`,
already free since this project uses neither Secure Boot nor Flash
Encryption). Left unburned at factory test, it can be burned exactly once
later, whenever the first flag needs setting, without touching or
invalidating the identity block in `BLOCK3`. A board that's never had a flag
block burned simply reads as `flags == 0`, indistinguishable from "burned
with no flags set". This was validated experimentally alongside the rest of
this design (burning `BLOCK3` and `BLOCK4` are independent operations — one
doesn't affect the other's RS checksum) but isn't implemented, since there's
no current consumer for it.

## Summary

- [x] `ud_hw_identity_t` struct and a read helper are defined in firmware
      (`HardwareVersion.hpp`).
- [x] Firmware logs hardware generation, revision, manufacturer ID, batch,
      and serial at boot.
- [x] A burn tool (`tools/efuse_burn.py`) is added for board test; it wraps a
      documented `espefuse burn-block-data` invocation.
- [x] Unburned boards: magic mismatch → log message, hardware version treated
      as unknown (`getHardwareVersion()` returns `std::nullopt`).
- [x] Device selection (`startDeviceBasedOnHardware()` in `main.cpp`) prefers
      the eFuse identity over MAC prefix matching when present, on the
      ESP32-C6 (Carrot) target.
- [x] `tools/test/test_efuse_burn.py` covers the burn tool against `--virt`
      and runs in CI (`embedded-test` job).
- [ ] `flags` / `FLAG_RMA` — deferred, see above.
