# Bluetooth (BLE) Specification

## Overview

Ugly Duckling devices expose a BLE peripheral using **ESP-IDF's NimBLE stack**.
The peripheral advertises continuously and accepts one connection at a time; on
disconnect it immediately restarts advertising.

Platforms:

| Platform | SoC      | BLE Support    |
| -------- | -------- | -------------- |
| Carrot   | ESP32-C6 | Yes            |
| Spinach  | ESP32-S3 | No (see below) |

### Platform support decision

BLE is **fully disabled on Spinach** (pre-MK10 devices — MK5 through MK9), both
at the sdkconfig level (`CONFIG_BT_ENABLED=n` in `sdkconfig.spinach.defaults`,
overriding `sdkconfig.defaults`' `CONFIG_BT_ENABLED=y` for this platform only)
and in application code (everything depending on NimBLE is compiled out under
`#ifdef CONFIG_BT_NIMBLE_ENABLED` — see [Driver Architecture](#driver-architecture)).

Rationale: proper BLE support (WiFi provisioning, local-only mode, etc.) matters
for **new** devices going forward — Carrot (MK10+). Pre-MK10 devices keep their
existing provisioning path (SoftAP/USB) and data transfer path (WiFi/MQTT) and
don't need BLE at all, so there's no reason to carry it — including debugging
ESP32-S3-specific BLE controller quirks (community reports suggest its BLE
controller shares lineage with the older, more constrained ESP32-C3 rather than
the newer C6/H2 stack, and that connectable extended advertising is less
reliable there — not confirmed on this codebase, since the investigation was
abandoned in favor of this decision). Disabling BLE at compile time also
recovers flash/RAM otherwise spent on `libble_app.a` and the NimBLE host task on
these older, more resource-constrained boards.

If a pre-MK10 device ever needs BLE after all, re-enable
`CONFIG_BT_ENABLED`/`CONFIG_BT_NIMBLE_ENABLED` (and the rest of the Bluetooth
block from `sdkconfig.defaults`) in `sdkconfig.spinach.defaults`, then revisit
the ESP32-S3 connectable-extended-advertising question above before shipping.

**Alternative considered — drop back to legacy advertising:** switching from
BLE 5.0 extended advertising to legacy advertising (`ble_gap_adv_*` instead of
`ble_gap_ext_adv_*` — see the [compatibility note](#advertising) above) is a
separate option, not chosen here but still open, primarily to address Android
extended-advertising support being a per-chipset hardware property rather than
guaranteed by API level. It would mean reintroducing the primary-ad/scan-response
split (31 bytes each) since legacy PDUs can't carry today's merged payload
(name + 3×16-bit UUIDs + 1×128-bit UUID). It's also plausible (though
unconfirmed — see above) that legacy PDUs sidestep the ESP32-S3
connectable-extended-advertising reliability question entirely, since that
issue is specific to the extended, non-legacy PDU path. **If we do make this
switch, Spinach/ESP32-S3 BLE support is worth reconsidering** — the S3
rationale for staying with extended advertising falls away once legacy PDUs
are back in play, though the "why bother, older devices don't need it"
argument from the decision above would still need to be re-evaluated
independently.

---

## Identity and Addressing

Each device generates a **static random address** (top two bits of MSB set to
`11` per Bluetooth spec) on first boot and persists it in NVS (namespace `ble`,
key `addr`). This gives a stable identity across reboots without requiring a
public address.

The **device name** used in the GAP advertising payload equals the network
hostname (e.g. `ugly-duckling-aabbcc`).

---

## Advertising

Uses **BLE 5.0 extended advertising** (`ble_gap_ext_adv_*`, `CONFIG_BT_NIMBLE_EXT_ADV=y`)
rather than legacy advertising. A single extended AD payload has room for up
to 1650 bytes, so — unlike legacy advertising's 31-byte primary + 31-byte
scan-response split — flags, device name, and all service UUIDs fit in one
payload; there is no separate scan response.

| PDU                     | Contents                                                        |
| ----------------------- | --------------------------------------------------------------- |
| Extended advertising ad | Flags + device name + complete list of 16-bit and 128-bit UUIDs |

Service UUIDs included:

| UUID                                   | Service                        |
| -------------------------------------- | ------------------------------ |
| 0x180A                                 | Device Information (DIS)       |
| 0x180F                                 | Battery (BAS)                  |
| 0x1805                                 | Current Time (CTS)             |
| `100D32C7-A4E6-4F72-8D7A-A61871CE4FD6` | Ugly Duckling Service (custom) |

> **Compatibility note:** extended advertising is a real BLE 5.0 controller
> capability, not universally supported by every phone or scanner app. iOS
> requires **iPhone XS or later, iOS 13+** (iPhone X, released the same year
> as BT5 hardware shipped, is _not_ included — the cutover is specifically
> XS/XR); Apple caps extended AD data it reads at 124 bytes. Android has the
> scanning API since 8.0 (API 26), but actual support is a **per-chipset
> hardware property** (`BluetoothAdapter.isLeExtendedAdvertisingSupported()`)
> independent of Android version — some current-generation budget phones still
> return `false`. In practice, **nRF Connect failed to show the device name**
> (`N/A`) when we switched to extended advertising, while **LightBlue and the
> Ugly Duckling companion app (iOS) both work correctly** — the failure mode is
> app/OS-specific parsing support, not malformed advertising data. If broad
> Android compatibility becomes a requirement, NimBLE supports running a
> second, legacy-PDU advertising set concurrently (`ble_gap_ext_adv_params.legacy_pdu`,
> or a second advertising instance via `CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES`)
> — not implemented, since the companion app is the primary provisioning path.

### Burst-mode advertising (power)

Advertising fires in **single-event bursts** roughly every 2 s, rather than
advertising continuously with a 2 s interval. See
[WiFi/BLE Coexistence & Power](#wifible-coexistence--power) below for why —
in short, the ESP32-C6's WiFi/BLE coexistence scheduler picks its (expensive)
scheme based on whether the controller's advertising _state_ is enabled at
all, not on the actual on-air event cadence, so continuous advertising (even
at a slow interval) keeps the coexistence scheduler engaged and the chip
waking from light sleep constantly.

Mechanics (`BleDriver::startAdvertising()` /
`BleDriver::configureAndSetData()`):

1. Advertising parameters and AD payload are configured **once** via
   `ble_gap_ext_adv_configure()` + `ble_gap_ext_adv_set_data()` — both are HCI
   round-trips to the controller, not local struct updates, and neither
   changes between bursts, so repeating them is pure (and, as discovered, not
   harmless) overhead.
2. Each burst is a single advertising event: `ble_gap_ext_adv_start(instance, 0, 1)`
   (`max_events=1`), at the standard "fast advertising interval 1" range
   (`BLE_GAP_ADV_FAST_INTERVAL1_MIN`/`_MAX`, 30-60 ms) so the one event fires
   promptly.
3. On `BLE_GAP_EVENT_ADV_COMPLETE`, a `ble_npl_callout` (initialized against
   NimBLE's own host event queue, so its callback runs directly on the host
   task with no cross-task marshaling) re-arms for ~2 s, then restarts the
   next burst.

Net over-the-air behavior is unchanged from continuous 2 s-interval
advertising — one connectable event roughly every 2 s — only the controller's
state _between_ events differs (idle instead of continuously enabled).

---

## Standard GATT Services

### Device Information Service (DIS — 0x180A)

Readable with any BLE scanner (nRF Connect, LightBlue, etc.).

| Characteristic    | Value source                             |
| ----------------- | ---------------------------------------- |
| Manufacturer Name | `"Cornucopia Machines"` (compile-time)   |
| Model Number      | `"Ugly Duckling <model><rev>"` (runtime) |
| Firmware Revision | Firmware version string (runtime)        |
| Serial Number     | MAC address string (runtime)             |

Enabled by sdkconfig:

```
CONFIG_BT_NIMBLE_SVC_DIS_MANUFACTURER_NAME=y
CONFIG_BT_NIMBLE_SVC_DIS_FIRMWARE_REVISION=y
CONFIG_BT_NIMBLE_SVC_DIS_SERIAL_NUMBER=y
```

### Battery Service (BAS — 0x180F)

Reports the current battery level as an integer percentage (0–100).
`BleDriver::setBatteryLevel()` is called periodically by `Device.hpp` from the
`BatteryManager`.

### Current Time Service (CTS — 0x1805)

Allows a BLE central (e.g. a phone app) to push the current UTC time to the
device on connect, avoiding NTP dependency in environments without internet
access.

Callbacks wired in `Device.hpp`:

| Callback             | Behaviour                                      |
| -------------------- | ---------------------------------------------- |
| `fetch_time_cb`      | Returns system clock (UTC) when read           |
| `set_time_cb`        | Calls `RtcDriver::setTime()` with received UTC |
| `local_time_info_cb` | Reports UTC / no DST                           |
| `ref_time_info_cb`   | Reports source unknown, accuracy unknown       |

---

## Driver Architecture

`BleDriver` (`components/kernel/src/drivers/BleDriver.hpp`) owns the NimBLE
lifecycle:

- `BleDriver` itself is a no-op base class (`getStatus()` always returns
  `Disabled`) with no NimBLE dependency, so it's always available. Its subclass
  `NimBleDriver` — and every NimBLE header it needs — is compiled only under
  `#ifdef CONFIG_BT_NIMBLE_ENABLED`, since on platforms where the option is off
  (Spinach — see [Platform support decision](#platform-support-decision)) the
  `bt` component doesn't even expose the nimble/host include paths. `Device.hpp`
  mirrors the same `#ifdef`: it only instantiates `NimBleDriver` (gated further
  by `settings->bleEnabled` at runtime) inside the guard, falling back to the
  no-op `BleDriver` unconditionally outside it.
- Constructed in `Device.hpp` before WiFi, passing device name, model, firmware
  version, serial number, and an `NvsStore` for address persistence.
- Constructor errors (NimBLE init, GATT service registration, GAP/DIS field
  setters) **throw** rather than log-and-continue — a device that can't stand
  up its GATT server or advertise isn't usable, so it fails fast at boot.
  NimBLE host functions return their own `int` error codes (`BLE_HS_*`), not
  `esp_err_t`, so a small `throwOnBleError()` helper is used alongside
  `ESP_ERROR_THROW` (for the genuine `esp_err_t`-returning calls, e.g.
  `nimble_port_init()`). Runtime paths (notify, restart-advertising-after-
  disconnect) still log-and-continue, since a transient failure there is
  expected and recoverable.
- A FreeRTOS task (`hostTask`) runs the NimBLE host loop.
- `onSync` → calls `ble_hs_id_set_rnd()` then `startAdvertising()`.
- `startAdvertising()` / `configureAndSetData()` implement burst-mode
  advertising — see [Burst-mode advertising (power)](#burst-mode-advertising-power).
- `gapEventCallback` handles connect/disconnect (restarts advertising) and
  `BLE_GAP_EVENT_ADV_COMPLETE` (re-arms the next burst).
- `BleStatus` enum: `Idle | Resetting | Error | Advertising | Connected`.
- The single global `instance` pointer allows static C callbacks to dispatch
  back to the driver instance.

---

## WiFi/BLE Coexistence & Power

Enabling BLE (`DeviceSettings.bleEnabled = true`) on MK11 (ESP32-C6) originally
pushed average current from ~3 mA to ~13 mA — far more than periodic
advertising should cost. Root-causing this took a long investigation across
two builds — this codebase and a scratch copy of Espressif's
`nimble/power_save` example — since the SoC and IDF version were never the
problem; specific configuration and application-level choices were. Two
independent root causes were found and fixed; a third issue (a crash)
surfaced while fixing the second and was fixed in turn.

### Root cause 1 — `CONFIG_ESP_WIFI_ENHANCED_LIGHT_SLEEP`

This option ("hardware-assisted beacon reception during light sleep: the WiFi
modem wakes autonomously for each beacon interval, keeping the CPU in light
sleep until actual data arrives"; C6-specific, requires
`SOC_PM_SUPPORT_BEACON_WAKEUP`) sounds purely beneficial, and was enabled
expecting a power improvement. Instead, with BLE also active, it made
`esp_light_sleep_start()` reject **100% of attempts** — confirmed via
`esp_pm`'s `light_sleep_counts`/`light_sleep_reject_counts` (`CONFIG_PM_PROFILING=y`)
staying at `light_sleep_counts:0` for the whole run — even with WiFi never
connected. The elevated current came from constantly attempting and aborting
light sleep, not from short-but-real sleep cycles. Measured impact on MK11:
~33 mA with the option enabled vs. ~4 mA disabled, BLE advertising in both
cases. Root-caused by bisecting a full `sdkconfig` diff against the
`power_save` reference example (which does not enable this option) down to
this single setting.

**Fix:** keep `CONFIG_ESP_WIFI_ENHANCED_LIGHT_SLEEP` disabled — see the
detailed comment in `sdkconfig.carrot.defaults`. Not filed upstream yet (TODO:
reproduce on a minimal example and file an ESP-IDF issue). The mutually
exclusive `SLP_SAMPLE_BEACON_FEATURE` (beacon timing calibration via sampling,
rather than hardware wakeup) is an untested, separate avenue for the same
"receive beacons without a full CPU wake" goal — worth investigating if beacon
lost handling ever needs improving, but do not assume it has the same problem.

### Root cause 2 — coexistence scheduler keyed on advertising _state_, not on-air activity

With root cause 1 fixed, WiFi-only and BLE-only were both cheap (~2 mA and
~1.3 mA respectively, even with BLE's 2 s advertising interval) — but WiFi
**and** BLE together still drew ~13 mA. The deciding experiment: BLE advertising
alone was cheap regardless of interval, so the expense wasn't about how often
an actual advertising PDU went out — it was specifically about having _both_
radios active together. That pointed at the coexistence (coex) scheduler.

Espressif's coexistence documentation confirms the mechanism: when WiFi STA is
connected, the coexistence period is anchored to **TBTT** (Target Beacon
Transmission Time — the AP's beacon interval, typically ~100 ms) —
independent of BLE's `listen_interval` or advertising interval. This matched
a periodic ETSTimer pair found via `esp_timer_dump()` firing roughly every
~105 ms. The controller was, in effect, choosing its (expensive) coexistence
scheme based on whether BLE advertising was _enabled at all_, not on the
actual cadence of advertising events — so even a slow, infrequent advertiser
kept the coex scheduler engaged continuously, waking the chip from light sleep
every ~2.8 ms to service RF time slices.

**Fix:** burst-mode advertising — see
[Burst-mode advertising (power)](#burst-mode-advertising-power) above. Firing
single-event bursts and leaving the controller idle (not "enabled") between
bursts starves the coex scheduler's reason to keep the chip awake, since the
scheduler reacts to state, not cadence. Measured: ~13 mA continuous
advertising vs. ~3-3.25 mA bursty, WiFi + BLE both active in both cases.

### A crash along the way — NimBLE host crash under WiFi+MQTT+TLS load

The first burst-mode implementation re-ran the _entire_ advertising setup
(`ble_gap_ext_adv_configure()` + mbuf alloc + `ble_gap_ext_adv_set_data()` +
`ble_gap_ext_adv_start()`) on every ~2 s burst. `ble_gap_ext_adv_configure()`
and `ble_gap_ext_adv_set_data()` are real HCI round-trips to the controller,
not local struct updates — and neither the advertising parameters nor the AD
payload (name, UUIDs) ever change between bursts, so this was pure redundant
HCI traffic, piling onto an already-busy HCI event path. Under real WiFi STA,
MQTT, and TLS handshake load, this crashed the NimBLE host a few seconds
after boot:

```
assertion:event
line:103,function:npl_freertos_event_init
Guru Meditation Error: Core  0 panic'ed (Store access fault).
...
ble_hs_event_rx_hci_ev at .../nimble/host/src/ble_hs.c:644
```

— NimBLE's own HCI-event-receive path handed `npl_freertos_event_init` a
NULL/bad event pointer, consistent with NPL/HCI event pool exhaustion
(`CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT`, default 30) once the host task got
starved during the CPU-heavy TLS handshake while HCI events kept accumulating
from the redundant per-burst reconfiguration.

**Fix**, in `BleDriver::startAdvertising()` / `configureAndSetData()`:

1. Configure the advertising set (params + AD payload) **once**, guarded by
   `advConfigured`. Every burst after the first calls only
   `ble_gap_ext_adv_start()`.
2. Replaced the burst-restart `esp_timer` with a `ble_npl_callout`
   initialized against NimBLE's own host event queue
   (`nimble_port_get_dflt_eventq()`) — its callback runs directly on the host
   task, removing a cross-task hop (esp_timer task → manual marshaling → host
   task) that was a contributing factor under load.
3. `CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT` 30→60 and
   `CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT` 8→16, as headroom against
   the same failure mode recurring under other bursts of HCI activity — not
   the fix itself, insurance alongside it.

### Investigative dead ends

Ruled out during root cause 2's investigation, on this codebase:

- WiFi/coexistence in general — reproduces identically with WiFi driver never
  initialized at all (rules out coexistence as _root cause 1_'s mechanism;
  root cause 2 specifically requires both radios active, see above).
- NimBLE `Central`/`Observer` roles — disabling both (`CONFIG_BT_NIMBLE_ROLE_CENTRAL=n`,
  `CONFIG_BT_NIMBLE_ROLE_OBSERVER=n`; kept disabled regardless, since we never
  scan or connect out) made no difference. Also rules out background scanning
  as a contributor.
- GATT services (DIS/BAS/CTS/custom) — disabling all of them, bare advertising
  only, made no difference.
- Legacy vs. extended advertising API choice — made no difference to the power
  problem (extended advertising was kept anyway, for larger-payload headroom;
  see the compatibility note above).
- `CONFIG_PM_LIGHTSLEEP_RTC_OSC_CAL_INTERVAL` (16 vs. default 1) — changed the
  _shape_ of the numbers (fewer wakeups, worse sleep ratio) but not the
  underlying problem.
- `CONFIG_PM_LIGHT_SLEEP_CALLBACKS` — disabling entirely made no difference.
- A deliberately slow `esp_pm` light-sleep callback (busy-wait in `enter_cb`,
  tested on the reference example) — did not reproduce the reject storm,
  ruling out light-sleep-callback overhead as a mechanism.
- PM lock hold times (`CONFIG_PM_PROFILING=y`) and `esp_timer` usage
  (`CONFIG_ESP_TIMER_PROFILING=y`) — nothing held/scheduled anywhere near the
  observed wakeup rate.
- Per-task CPU time (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`) — no single
  task was CPU-hogging; the reject loop runs inside the idle task itself
  (`vApplicationSleep`), which is why task-level stats couldn't show a
  "culprit task."

Ruled out during root cause 2's investigation, on the `nimble/power_save`
reference example (a separate, scratch copy used to isolate application code
from `sdkconfig`):

- Initial hypothesis carried over from the real-firmware investigation was
  that BLE's controller wakes on an inherent ~1 ms hardware cadence regardless
  of anything else. Once BLE-alone measured a clean 255-600 µA on the
  reference example, that theory didn't hold — the culprit was
  coexistence-specific, only appearing when both radios were live.
- `esp_wifi_set_ps(WIFI_PS_MAX_MODEM)` + `listen_interval=20` — helped a
  little (13 mA → ~12 mA) but nowhere near enough.
- `esp_coex_preference_set(ESP_COEX_PREFER_BT)` — measured essentially zero
  effect (lock/sleep numbers stayed within noise of the default
  `PREFER_BALANCE`).
- Lowering max CPU frequency 160→80 MHz — negligible effect; wakeup
  _frequency_ is what mattered, not clock speed during each wake.
- Espressif's coexistence doc's "Wi-Fi Connectionless Modules Coexistence"
  section (Window/Interval parameters) looked promising but turned out to be
  about ESP-NOW/DPP/FTM, not a plain WiFi-STA + BLE-advertising setup — a red
  herring.

### Open follow-ups

- [Issue #593](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/593) —
  `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP` was pulled in alongside
  `ESP_WIFI_ENHANCED_LIGHT_SLEEP` during the original investigation and never
  isolated on its own; still commented out (`# TODO Experiment with this
separately`) in `sdkconfig.defaults`.
- File an upstream ESP-IDF issue for the `ESP_WIFI_ENHANCED_LIGHT_SLEEP` +
  BLE interaction once reproduced on a minimal example.
- `SLP_SAMPLE_BEACON_FEATURE` (see root cause 1) as an alternative to the
  disabled hardware beacon-wakeup option — unexplored.

---

## Ugly Duckling Service (Custom GATT)

> Phases 1–2 shipped on `bluetooth/enable`. Phases 3–5 pending.

### Implementation checklist

#### Phase 1 — WiFi scan (firmware)

- [x] Assign a permanent 128-bit UUID for the Ugly Duckling Service
- [x] Add the custom GATT service skeleton to `BleDriver` (NimBLE `ble_gatt_svc_def` table)
- [x] Add the custom service UUID to the scan-response PDU in `startAdvertising()`
- [x] Implement the **WiFi Scan Results** characteristic (Notify only)
  - [x] Trigger `esp_wifi_scan_start()` via `scan` command on WiFi Control; handle APSTA mode if SoftAP is active
  - [x] Deliver each AP as an individual Notify (`{"ssid":…,"rssi":…,"authMode":…,"wifiGen":…}`)
  - [x] Send one empty Notify as end-of-stream sentinel after the last AP

#### Phase 2 — WiFi credentials (firmware)

- [x] Implement the **WiFi Status** characteristic (Read + Notify)
  - [x] Report `unconfigured`, `connecting`, `connected`, `failed:<reason>`, `disabled`
  - [x] Notify on every state change
- [x] Implement the **WiFi Credentials** characteristic (Write Without Response)
  - [x] Parse `{ssid, password}` JSON
  - [x] Validate inputs; map errors to `failed:<reason>` in WiFi Status
  - [x] Store credentials in NVS and trigger `WiFiDriver` reconnect
- [x] Implement the **WiFi Control** characteristic (Write Without Response)
  - [x] Handle `scan` command (trigger WiFi scan; results delivered via WiFi Scan Results notifications)
  - [x] Handle `disconnect` command
  - [x] Handle `disable` command (stub for WiFi-less mode)

#### Phase 3 — security (firmware)

- [ ] Enable BLE link-layer encryption (Just Works: `sm_sc = 1`, `sm_bonding = 1`)
- [ ] Mark WiFi Credentials and WiFi Status with `BLE_GATT_CHR_F_WRITE_ENC` / `BLE_GATT_CHR_F_READ_ENC`

#### Phase 4 — SoftAP removal

- [ ] Remove `network_prov_scheme_softap` and `startProvisioning()` from `WiFiDriver`
- [ ] Remove `configPortalRunning` state and associated LED pattern from `KernelStatusTask`
- [ ] Remove `NETWORK_PROV_EVENT` handler registration

#### Phase 5 — OOB security (future, requires factory provisioning flow)

- [ ] Define eFuse field layout for the 128-bit OOB key
- [ ] Add factory provisioning script to burn OOB key into eFuse and generate QR code label
- [ ] Enable OOB pairing in NimBLE (`sm_mitm = 1`, OOB data from eFuse)
- [ ] Update companion app to scan QR code before pairing

A single custom GATT service ("Ugly Duckling Service") will cover all
device-specific functionality, starting with WiFi provisioning. Using one
service avoids PDU size pressure: each advertising PDU is at most 31 bytes, and
a 128-bit UUID consumes 16 of them, leaving almost no room for additional
custom UUIDs in the scan response.

### Service UUID

| Item                  | UUID                                   |
| --------------------- | -------------------------------------- |
| Ugly Duckling Service | `100D32C7-A4E6-4F72-8D7A-A61871CE4FD6` |

The service UUID is included in the scan-response PDU so apps can filter for
Ugly Duckling devices without connecting first.

### Intended user flow (WiFi provisioning)

1. User opens a companion app (or future Web Bluetooth UI).
2. App connects to the device over BLE and discovers the Ugly Duckling Service.
3. App reads the **WiFi Status** characteristic to check if the device is
   already provisioned.
4. If not provisioned (or re-provisioning), app subscribes to **WiFi Scan
   Results** notifications, then writes `scan` to **WiFi Control** to trigger a
   scan. Each AP arrives as a separate notification; an empty notification
   signals the end of the list.
5. App subscribes to **WiFi Status** notifications.
6. App writes the **WiFi Credentials** characteristic with the chosen SSID and
   password.
7. App watches **WiFi Status** notifications for the outcome: `connected`, or
   `failed:<reason>` (covering both immediate validation errors and async
   connection failures).

### Characteristics

All characteristic UUIDs are 16-bit, scoped to the Ugly Duckling Service. Each also exposes a read-only **User Description** descriptor (0x2901) so BLE scanner apps (LightBlue, nRF Connect) display a human-readable label alongside the UUID.

| Characteristic    | UUID     | Properties             | Description                                                                                                                                                                                                                                                      |
| ----------------- | -------- | ---------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| WiFi Scan Results | `0x0001` | Notify                 | Delivers scan results as individual notifications — one per AP (`{"ssid":…,"rssi":…,"authMode":…,"wifiGen":…}`) — followed by one empty notification as an end-of-stream sentinel. Read is not supported. A scan is triggered by writing `scan` to WiFi Control. |
| WiFi Status       | `0x0002` | Read, Notify           | Reports current WiFi state as a short string: `unconfigured`, `connecting`, `connected`, `failed:<reason>`, or `disabled`. Notified on every state change. Failure reasons include `ssid_too_long`, `password_too_long`, `auth_failed`, `ap_not_found`.          |
| WiFi Credentials  | `0x0003` | Write Without Response | Accepts `{ssid, password}` as JSON. Device validates and initiates a connection attempt; outcome reported via WiFi Status.                                                                                                                                       |
| WiFi Control      | `0x0004` | Write Without Response | Accepts a command string: `scan` (trigger a WiFi scan; results delivered via WiFi Scan Results notifications), `disconnect` (drop current connection), or `disable` (turn off WiFi entirely, for future WiFi-less mode).                                         |

### Security

Ugly Duckling devices have no display or keyboard, so passkey-based pairing is
not available. Two options, in order of preference:

1. **OOB (Out of Band) pairing** — a 128-bit key burned into eFuse during
   factory provisioning is encoded as a QR code on the device label. The phone
   app scans the QR to obtain the OOB key before pairing. This gives full MITM
   protection without a display and survives factory resets (unlike NVS).
   Requires a factory provisioning flow that burns the key into eFuse and
   produces the matching label.

2. **Just Works** — the connection is encrypted (passive eavesdropping
   prevented) but there is no MITM protection. An attacker physically nearby
   during the brief provisioning window could intercept. Acceptable as an
   initial implementation given the home-use threat model.

In both cases, the WiFi Credentials and WiFi Status characteristics are marked
with `BLE_GATT_CHR_F_WRITE_ENC` / `BLE_GATT_CHR_F_READ_ENC` so NimBLE rejects
access on unencrypted connections. NimBLE security manager config:
`sm_sc = 1` (Secure Connections), `sm_bonding = 1`; add `sm_mitm = 1` when OOB
is enabled.

**Plan:** ship with Just Works; switch to OOB once the factory provisioning
flow and companion app QR scanning are in place.

### Coexistence with SoftAP provisioning

During the transition period, BLE provisioning will run **in parallel** with
the existing SoftAP-based `network_prov_mgr` flow. Both radios are independent
so there is no inherent conflict. Notes:

- Credential writes from both paths target the same NVS store; whichever
  completes first wins and `WiFiDriver` reconnects normally.
- The BLE WiFi scan characteristic requires STA (or APSTA) mode. If SoftAP
  provisioning is active (AP mode only), the scan must either be deferred or
  the driver must switch to APSTA mode briefly.
- Once BLE provisioning is stable and a companion app exists, the SoftAP path
  will be removed.

### Implementation notes (to be refined)

- WiFi scan runs via `esp_wifi_scan_start()`. On ESP32, a scan can run while
  associated to an AP (passive or brief active scan on the home channel), so an
  existing connection need not be dropped.
- The WiFi Scan Results characteristic is Notify-only (no Read). A scan is
  triggered by writing `scan` to the WiFi Control characteristic. The firmware
  sends one Notify per discovered AP — each a self-contained JSON object
  `{"ssid":…,"rssi":…,"authMode":…,"wifiGen":…}` — followed by one **empty Notify** (zero
  bytes) as an end-of-stream sentinel. The client appends each AP to a list and
  finalises on the empty sentinel. No string assembly or re-parsing is required.
  `wifiGen` is the highest 802.11 generation the AP advertises: `1`=b, `3`=g,
  `4`=n (WiFi 4), `6`=ax (WiFi 6). Derived from the PHY bitfields in
  `wifi_ap_record_t`; `phy_11ac` is not exposed by ESP-IDF (5 GHz not supported).
  Each AP's JSON is at most ~90 bytes (32-byte SSID + longest authMode string + wifiGen),
  which fits comfortably within the MTU negotiated by any modern phone (≥ 256);
  MTU exchange is expected before subscribing.
- Credentials are stored in NVS so `WiFiDriver`'s existing reconnect logic
  picks them up on subsequent boots. The `network_prov_mgr` NVS format will be
  reused if compatible; otherwise credentials go directly under a known NVS
  namespace.
- `WiFi Control: disable` lays the groundwork for a future **WiFi-less mode**
  where the device operates solely over BLE (no MQTT, no NTP). Full support for
  that mode is out of scope for now but the characteristic should be reserved.
