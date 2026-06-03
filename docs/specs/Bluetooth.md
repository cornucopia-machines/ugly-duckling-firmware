# Bluetooth (BLE) Specification

## Overview

Ugly Duckling devices expose a BLE peripheral using **ESP-IDF's NimBLE stack**.
The peripheral advertises continuously and accepts one connection at a time; on
disconnect it immediately restarts advertising.

Platforms:

| Platform | SoC      | BLE Support |
| -------- | -------- | ----------- |
| Carrot   | ESP32-C6 | Yes         |
| Spinach  | ESP32-S3 | Yes         |

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

Two PDUs are sent on each advertising interval:

| PDU           | Contents                              |
| ------------- | ------------------------------------- |
| Primary ad    | Flags + device name (up to 26 bytes)  |
| Scan response | Complete list of 16-bit service UUIDs |

Service UUIDs included in the scan response:

| UUID                                   | Service                        |
| -------------------------------------- | ------------------------------ |
| 0x180A                                 | Device Information (DIS)       |
| 0x180F                                 | Battery (BAS)                  |
| 0x1805                                 | Current Time (CTS)             |
| `100D32C7-A4E6-4F72-8D7A-A61871CE4FD6` | Ugly Duckling Service (custom) |

The three standard 16-bit UUIDs together occupy 3 × 2 + 1 (type) + 1 (length)
= 8 bytes. The one 128-bit custom UUID adds 16 + 2 = 18 bytes. Total: 26 bytes
out of the 31-byte scan-response PDU — no room for additional custom UUIDs,
which is why all custom functionality goes into a single service.

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

- Constructed in `Device.hpp` before WiFi, passing device name, model, firmware
  version, serial number, and an `NvsStore` for address persistence.
- A FreeRTOS task (`hostTask`) runs the NimBLE host loop.
- `onSync` → calls `ble_hs_id_set_rnd()` then `startAdvertising()`.
- `gapEventCallback` handles connect/disconnect and restarts advertising on
  disconnect.
- `BleStatus` enum: `Idle | Resetting | Error | Advertising | Connected`.
- The single global `instance` pointer allows static C callbacks to dispatch
  back to the driver instance.

---

## Planned: Ugly Duckling Service (Custom GATT)

> Status: **in progress**

### Implementation checklist

#### Phase 1 — WiFi scan (firmware)

- [x] Assign a permanent 128-bit UUID for the Ugly Duckling Service
- [x] Add the custom GATT service skeleton to `BleDriver` (NimBLE `ble_gatt_svc_def` table)
- [x] Add the custom service UUID to the scan-response PDU in `startAdvertising()`
- [x] Implement the **WiFi Scan Results** characteristic (Read + Notify)
  - [x] Trigger `esp_wifi_scan_start()` on read; handle APSTA mode if SoftAP is active
  - [x] Encode results as JSON array `[{ssid, rssi, authMode}]`
  - [x] Handle chunked transfer for payloads larger than the negotiated MTU
  - [x] Notify connected client when scan completes

#### Phase 2 — WiFi credentials (firmware)

- [ ] Implement the **WiFi Status** characteristic (Read + Notify)
  - [ ] Report `unconfigured`, `connecting`, `connected`, `failed:<reason>`, `disabled`
  - [ ] Notify on every state change
- [ ] Implement the **WiFi Credentials** characteristic (Write Without Response)
  - [ ] Parse `{ssid, password}` JSON
  - [ ] Validate inputs; map errors to `failed:<reason>` in WiFi Status
  - [ ] Store credentials in NVS and trigger `WiFiDriver` reconnect
- [ ] Implement the **WiFi Control** characteristic (Write Without Response)
  - [ ] Handle `disconnect` command
  - [ ] Handle `disable` command (stub for WiFi-less mode)

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

| Item                             | UUID                                   |
| -------------------------------- | -------------------------------------- |
| Ugly Duckling Service            | `100D32C7-A4E6-4F72-8D7A-A61871CE4FD6` |
| WiFi Scan Results characteristic | `0x0001` (16-bit, within the service)  |

The service UUID is included in the scan-response PDU so apps can filter for
Ugly Duckling devices without connecting first.

### Intended user flow (WiFi provisioning)

1. User opens a companion app (or future Web Bluetooth UI).
2. App connects to the device over BLE and discovers the Ugly Duckling Service.
3. App reads the **WiFi Status** characteristic to check if the device is
   already provisioned.
4. If not provisioned (or re-provisioning), app reads (or subscribes to)
   **WiFi Scan Results** to get a list of nearby SSIDs.
5. App subscribes to **WiFi Status** notifications.
6. App writes the **WiFi Credentials** characteristic with the chosen SSID and
   password.
7. App watches **WiFi Status** notifications for the outcome: `connected`, or
   `failed:<reason>` (covering both immediate validation errors and async
   connection failures).

### Characteristics (planned)

All UUIDs are 128-bit and TBD.

| Characteristic    | Properties             | Description                                                                                                                                                                                                                                             |
| ----------------- | ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| WiFi Scan Results | Read, Notify           | Triggers a WiFi scan on read; returns (and notifies) a compact JSON array of `{ssid, rssi, authMode}` for discovered APs. Large results may require chunked transfer (`Read Blob` or repeated Notify).                                                  |
| WiFi Credentials  | Write Without Response | Accepts `{ssid, password}` as JSON. Device validates and initiates a connection attempt; outcome reported via WiFi Status.                                                                                                                              |
| WiFi Status       | Read, Notify           | Reports current WiFi state as a short string: `unconfigured`, `connecting`, `connected`, `failed:<reason>`, or `disabled`. Notified on every state change. Failure reasons include `ssid_too_long`, `password_too_long`, `auth_failed`, `ap_not_found`. |
| WiFi Control      | Write Without Response | Accepts a command string: `disconnect` (drop current connection) or `disable` (turn off WiFi entirely, for future WiFi-less mode).                                                                                                                      |

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
- BLE MTU is negotiated per connection (default 23 bytes, up to ~512 bytes with
  MTU exchange). The scan results payload will likely exceed a single ATT PDU;
  options are chunked `Read Blob` reads, or a `Notify` stream with a simple
  framing marker.
- Credentials are stored in NVS so `WiFiDriver`'s existing reconnect logic
  picks them up on subsequent boots. The `network_prov_mgr` NVS format will be
  reused if compatible; otherwise credentials go directly under a known NVS
  namespace.
- `WiFi Control: disable` lays the groundwork for a future **WiFi-less mode**
  where the device operates solely over BLE (no MQTT, no NTP). Full support for
  that mode is out of scope for now but the characteristic should be reserved.
