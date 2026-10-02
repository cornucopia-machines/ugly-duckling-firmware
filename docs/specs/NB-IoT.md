# NB-IoT connectivity (Desert Lark)

## Summary

Add NB-IoT as an alternative to WiFi, using the Quectel BC660K-GL on the Desert Lark daughter
board (MK13+, Carrot only). The first goal is a demo: **telemetry and control over NB-IoT,
reaching the same MQTT broker, with no server-side changes.** Saving bandwidth, battery and
latency (DTLS + CID, CoAP) comes after that works.

Tracking issues:

- [#641](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/641) — firmware driver; bench notes from bring-up
- [#640](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/640) — move the console off UART0 (done, #675)
- [cornucopia-app#492](https://github.com/cornucopia-machines/cornucopia-app/issues/492) — end-state transport (CoAP over DTLS 1.2 + CID)
- [cornucopia-app#507](https://github.com/cornucopia-machines/cornucopia-app/issues/507) — device time acquisition (NITZ)
- Hardware design: [`ugly-duckling-hardware/docs/specs/nb-iot.md`](https://github.com/cornucopia-machines/ugly-duckling-hardware/blob/main/docs/specs/nb-iot.md)

## Terms

- **AT command** — text command sent to the modem over UART (`AT+CSQ\r`); the modem answers with
  result lines and a final `OK` / `ERROR`.
- **URC** (Unsolicited Result Code) — a line the modem sends on its own, not as a reply to a
  command: `+CEREG: 5` when registration changes, `+QIURC: "recv",0` when socket data arrives.
- **`+CEREG`** — *EPS network REGistration status* from 3GPP TS 27.007 (EPS = the LTE core
  network; NB-IoT registers through it). Siblings: `+CREG` (circuit-switched), `+CGREG` (GPRS).
- **PSM** (Power Saving Mode) — the modem stays registered but is unreachable for hours; it wakes
  only on its own schedule or when we send data.
- **eDRX** (extended Discontinuous Reception) — the modem sleeps but listens for paging at a fixed
  cycle (seconds to hours), so the network can still deliver downlink data with bounded latency.
- **ECL** (Coverage Enhancement Level, 0–2) — how much repetition the link needs; higher means
  weaker coverage, more airtime and more energy per byte.

## Key decisions

### esp-mqtt over an AT-socket transport, on esp_modem

The BC660K-GL has no PPP, so lwIP can't run over it. That leaves two ways to do MQTT:

1. **The modem's built-in MQTT client** (`AT+QMT*`). Light on the ESP32, but it would be a
   second MQTT stack next to esp-mqtt, with its own QoS, reconnect and TLS behavior, plus its own
   payload size limits. `MqttDriver` would need a parallel implementation.
2. **esp-mqtt over a custom `esp_transport`** built on the modem's TCP socket commands
   (`AT+QIOPEN` / `QISEND` / `QIRD`). `esp_mqtt_client_config_t.network.transport` already
   accepts a custom transport (today it's `nullptr` in `MqttDriver::configMqttClient()`), so
   `MqttDriver` itself stays almost untouched, and the bytes on the wire are the same as over WiFi.

**We go with option 2.** None of the protocols is ours to implement:

| Layer | Library |
| ----- | ------- |
| MQTT | esp-mqtt (already used) |
| TLS | mbedTLS (already used) |
| AT framing, command/response matching, URC dispatch, generic 3GPP commands | [`espressif/esp_modem`](https://components.espressif.com/components/espressif/esp_modem), command mode |
| AT socket → `esp_transport` glue | Adapted from esp_modem's [`modem_tcp_client`](https://github.com/espressif/esp-protocols/tree/master/components/esp_modem/examples/modem_tcp_client) example |
| BC660K-specific commands | Ours (`Bc660KDriver`) |

The `modem_tcp_client` example is exactly this setup: `tcp_transport_at.cpp` plugged into
esp-mqtt, a `sock_dce` layer, and per-chip `sock_commands_bg96.cpp` / `sock_commands_sim7600.cpp`.
The BG96 is a Quectel module using the same `QI*` socket commands as the BC660K. It is an
example rather than a component, so we vendor what we need; it is `Unlicense OR CC0-1.0`, so
there's no licensing constraint. What's left for us is the BC660K command file and the
lifecycle around it.

Alternatives looked at:
[TeschRenan/BC660K-GL](https://github.com/TeschRenan/BC660K-GL) (MIT, ESP-IDF, BC660K-specific)
brings its own UART and AT layers, duplicating esp_modem, and has no esp-mqtt transport. Not a
dependency, but a useful second reference for BC660K response formats. The other BC660K libraries
are Arduino or Mbed.

TLS runs on the ESP32 with mbedTLS, layered over the AT socket (the example shows how). That
keeps certificate handling identical to the WiFi path (`configServerCert`), keeps the TLS
endpoint ours rather than the modem firmware's, and is the same mbedTLS we will need for DTLS
later. RAM shouldn't be a problem: the WiFi path already runs MQTT over TLS, and a cellular build
doesn't start the WiFi driver, lwIP's WiFi netif or their buffers, so it should need less
internal RAM, not more.

### Leave room for other chipsets

```text
CellularDriver              ← owns the UART, power/wake, registration, networkReady, telemetry
  └─ CellularModuleDriver   ← chipset-specific AT: init sequence, sockets, URC parsing, time
       └─ Bc660KDriver      ← esp_modem GenericModule subclass; Quectel QI*/QENG/QSCLK commands
AtSocketTransport           ← esp_transport for esp-mqtt, talks only to CellularModuleDriver's socket API
```

Supporting another modem then means adding another `CellularModuleDriver`, the same way the
example adds `sock_commands_*.cpp` per chip.

### Choosing WiFi or NB-IoT

For the prototype, a CMake setting `-DUD_CONNECTIVITY=WIFI|CELLULAR` (default `WIFI`; same
mechanism as `UD_DEBUG` and `UD_PM_DIAGNOSTICS`, built in its own directory such as
`build-carrot-cellular`) switches `initConnectivity()` between the WiFi and cellular drivers. One
setting with named values, rather than a `UD_CELLULAR` boolean, makes the two options mutually
exclusive by construction, and leaves room for `BOTH` until this becomes a runtime choice. CMake
rejects anything else, and rejects `CELLULAR` on Spinach.

Both drivers set the same `networkReady` state, so `MqttDriver`, `TelemetryTask` and the rest
don't need to know which link is in use. Later this becomes a `network-config` setting, and
later still, use whichever link is available.

### UART0 console fallback

USB Serial/JTAG is the default console (#640). `-DUD_UART0_CONSOLE=1` puts it back on UART0
for early-boot debugging. The cellular driver checks `CONFIG_ESP_CONSOLE_UART_NUM == 0` at
startup and then **does not claim GPIO16/17**: it logs a warning and the device runs offline.
That way one build can still be debugged over the pogo header, at the cost of NB-IoT.

### Link quality in telemetry

The cellular build adds link-quality fields to device telemetry (RSRP, RSRQ, SINR, ECL, cell ID,
band, from `AT+QENG=0`), next to where the WiFi fields go. The server ignores fields it doesn't
know, so this needs no server change up front; server-side handling follows once the shape has
settled.

### OTA is off in cellular builds until stage 5

`HttpUpdater` downloads with `esp_http_client` over lwIP and waits on `WiFiDriver` directly, so it
can't work over the modem. In a `CELLULAR` build:

- the `update` handler answers a `firmware` entry with `RejectionCode::Unimplemented`, so the
  server stops retrying instead of the device rebooting into an update attempt that is bound to
  fail;
- the `http-update` command isn't registered.

## Plan

### Stage 1 — Free UART0 (#640)

- [x] Make USB Serial/JTAG the primary console in `sdkconfig.carrot.defaults`
- [x] Add opt-in `-DUD_UART0_CONSOLE=1` (`sdkconfig.uart0_console.defaults`)
- [x] Keep Wokwi simulations on the UART0 console (the serial monitor is wired to TX/RX)
- [x] Check that `DebugConsole` still works over USB Serial/JTAG
- [x] README: when and how to get the console back on UART0; unplug Desert Lark first
- [x] Merged (#675) and tested on a real device with the debug console enabled

### Stage 2 — Talk to the modem

Goal: on boot, the console shows replies to a few AT commands.

- [ ] Add `espressif/esp_modem` to `components/kernel/idf_component.yml`; check flash cost and that it builds without C++ exceptions
- [ ] `-DUD_CONNECTIVITY=WIFI|CELLULAR` CMake setting; in this stage `CELLULAR` only adds the modem driver next to WiFi
- [ ] Describe the modem UART pins per device model. MK13: TX/RX are swapped (hardware#93), so `tx = GPIO17`, `rx = GPIO16`; no reset GPIO (pin 5 is `EN`). MK14: open-drain `RESET_N` on GPIO4, pulsed once after boot
- [ ] Use UART1 through the GPIO matrix; refuse to start if the console is on UART0 (see above)
- [ ] `CellularDriver` skeleton, `CellularModuleDriver` interface, `Bc660KDriver : GenericModule`
- [ ] Wake: send `AT` repeatedly with backoff until `OK`; the first `AT` is consumed by the deep-sleep wake. Wrap every command in this sequence
- [ ] Boot init, sent every boot because these settings are not persisted: `AT+QSCLK`, `AT+QIDNSCFG`. Check once (persisted): `AT+CMEE=2`, `AT+QBAND`, `AT+CFUN`, `AT+QNBIOTEVENT`
- [ ] Log `ATI`, `AT+CGMR`, `AT+CIMI`, `AT+QCCID`, `AT+CSQ`, `AT+CEREG?`, `AT+QENG=0`
- [ ] Bench aid: AT passthrough from `DebugConsole` (type an AT command, see the reply), debug builds only
- [ ] Unit-test the response parsers in `test/unit-tests/`, using real captured responses as fixtures

### Stage 3 — MQTT over NB-IoT (`UD_CONNECTIVITY=CELLULAR`)

Goal: BOOT, SYNC (config/update request) and TELEMETRY reach the existing broker over NB-IoT.

- [ ] `initConnectivity()` builds `CellularDriver` instead of `WiFiDriver`; BLE WiFi provisioning hooks skipped
- [ ] Registration state machine driven by `AT+CEREG=3` URCs. Parse the two `+CEREG` shapes separately (the URC leads with `<stat>`, the read response with `<n>`), and surface the EMM reject cause (no coverage vs subscription refused)
- [ ] `networkConnecting` / `networkReady` set from registration + PDP context (cid 0)
- [ ] `AtSocketTransport` adapted from `tcp_transport_at.cpp`: `QIOPEN` (hostname directly or via `QIDNSGIP`, context 0 only), `QISEND`, `QIRD` in **buffer access mode**, `+QIURC: "recv"` / `"closed"` URCs
- [ ] Chunk writes at 2048 B (`QISEND`) and reads at 512 B (`QIRD`), and confirm on the bench (see open questions)
- [ ] mbedTLS over the AT transport; same server cert as WiFi
- [ ] `MqttDriver`: pass the custom transport in cellular builds; raise the keepalive (see open questions)
- [ ] OTA off: reject `firmware` entries with `Unimplemented`, don't register `http-update`
- [ ] Time: `RtcDriver` takes time from the modem instead of SNTP. NITZ (`AT+CTZU=1`, `AT+CCLK?`) first, `AT+QNTP` as fallback (parse the offset, it's not UTC). Note whether 1NCE delivers NITZ (for cornucopia-app#507)
- [ ] Telemetry: link-quality fields from `AT+QENG=0`
- [ ] Health check: MQTT keepalive / DNS lookup, **not** ping (ICMP is blocked on `SENSOR.NET`)
- [ ] Recovery: `AT+QRST` on a modem that stops responding; on MK14+ pulse `RESET_N` instead
- [ ] Keep the modem awake (no PSM/eDRX) and the TCP+TLS session up for this stage, to separate "does it work" from "does it sleep"
- [ ] Demo: BOOT, SYNC and TELEMETRY visible on the server from a Desert Lark board
- [ ] Measure: bytes per hour and per message type, connect time, TLS handshake bytes (see open questions for how)

### Stage 4 — Commands and updates, eDRX

Goal: commands and UPDATE messages sent from the server arrive with predictable latency, and
the modem sleeps between paging windows.

- [ ] Subscriptions over the AT transport (commands, UPDATE); check QoS 1 redelivery after a reconnect
- [ ] Enable eDRX (`AT+CEDRXS` / `AT+QEDRXCFG`), with a configurable cycle; log what the network grants (`+CEDRXP`)
- [ ] Measure 1NCE's NAT idle timeout and set the keepalive from it (see open questions)
- [ ] ESP32 light sleep with the modem attached. Waking on UART activity loses the first bytes of whatever woke us, so treat the wake itself as the signal: the socket is in buffer access mode, so after any UART wake, **poll `QIRD`** and re-query state, and never depend on that URC arriving intact. To confirm on the bench: the wake threshold, and that nothing besides socket data and `+CEREG` needs catching
- [ ] Measure command latency against the eDRX cycle, and average current
- [ ] Demo: valve override from the app reaches the device over NB-IoT

### Stage 5 — OTA over NB-IoT

- [ ] Decide the download path: HTTP(S) over the AT socket transport (esp_http_client has no custom transport hook, so it needs a thin HTTP client or the localhost-listener trick from the esp_modem example) vs the modem's HTTP commands, if the BC660K has them
- [ ] Resume interrupted downloads (HTTP range requests); a ~1.5 MB image is a large share of the 1NCE data budget, so retries must not start from zero
- [ ] Ship firmware update over NB-IoT end-to-end, including rollback; remove the stage 3 rejection
- [ ] Separately: modem firmware updates (Quectel DFOTA), if we need them

### Stage 6 — Use less data and battery, get lower latency

Order to be decided from the stage 3–4 measurements.

- [ ] TLS 1.3 PSK session resumption for MQTT (cheap stopgap from cornucopia-app#492)
- [ ] PSM between wakes for nodes that can tolerate command latency, with a cellular-specific wake/sync cadence
- [ ] Shorter field names in telemetry payloads (−35% measured, see cornucopia-app#492); needs server changes
- [ ] Server-side handling of the link-quality telemetry fields
- [ ] `network-config` setting to choose WiFi / NB-IoT, instead of the compile-time setting
- [ ] Use both: WiFi when available, NB-IoT fallback

### Stage 7 — CoAP over DTLS 1.2 + CID (cornucopia-app#492)

Only if MQTT turns out too expensive. Needs a server-side endpoint.

- [ ] Enable `CONFIG_MBEDTLS_SSL_PROTO_DTLS` and `MBEDTLS_SSL_DTLS_CONNECTION_ID`; measure flash cost
- [ ] UDP socket support in the AT transport (`QIOPEN` "UDP")
- [ ] Device keypair, BLE public-key registration, ECDH + HKDF PSK derivation
- [ ] CoAP client (libcoap) carrying `sync` / `update` / `telemetry`
- [ ] Compare per-wake bytes and radio-on time against stage 3–4

## Open questions

### `QISEND` / `QIRD` size limits

From the *BC660K-GL TCP/IP Application Note* v1.2 (`datasheets/.text/`):

- `AT+QISEND=<id>,<send_length>`: at most **2048 bytes** per command in text (raw) mode, 1024 in
  hex mode. In length-given mode the payload is raw bytes, which TLS records need.
- `AT+QIRD=<id>,<read_length>`: **1–512 bytes** per read.
- The modem buffers at most **2 KB** of received data per socket in buffer access mode. A large
  UPDATE has to be drained with several `QIRD`s as it arrives. We expect TCP flow control to hold
  back the rest rather than lose it; to be confirmed on the bench.
- `AT+QICFG="showlength",1` adds the remaining-length field to `QIRD` responses and the
  `recv` URC, so the transport knows how much more to read.

Steps:

1. Set the transport's write chunk to 2048 and its read chunk to 512 (the example's transport
   splits at its own buffer size). esp-mqtt writes whole packets, up to `buffer.out_size` (4 KB
   today), so writes will be split.
2. Confirm on the bench: send increasing sizes to an echo server (256 B → 4 KB), checking for
   `ERROR` and comparing with `AT+QISEND=<id>,0`, which reports bytes sent / acked / unacked.
   Then have the server send a SYNC/UPDATE larger than 2 KB.

### Keepalive and session expiry

- **Keepalive must be shorter than the carrier's NAT idle timeout.** Otherwise the NAT drops the
  TCP mapping, the broker's downlink goes nowhere, and we only notice on the next uplink. 1NCE's
  value isn't known yet; carrier NAT timeouts vary from minutes to hours.
- **Keepalive is the biggest fixed data cost.** esp-mqtt sends a PINGREQ every *keepalive/2*
  whether or not other traffic went out (it only resets the timer on CONNACK and PINGRESP, see
  `process_keepalive()` in `mqtt_client.c`). Today's 120 s keepalive means a ping a minute. Each
  ping + response over TLS/TCP is roughly 200 B with ACKs (estimate: 2 B MQTT + 29 B TLS record
  + 40 B TCP/IP, each way, plus ACKs), so about **290 KB/day**. That alone is twice the 1NCE
  budget (500 MB / 10 years ≈ 137 KB/day). At a 30 min keepalive (a ping every 15 min), it's
  about 19 KB/day.
- **So: keepalive ≈ 2 × (NAT timeout − margin)** (pings go out at half the keepalive), capped by
  how quickly we want to notice a dead link. Measure the NAT timeout: open an MQTT session with a
  very long keepalive, stay idle for increasing periods (5, 10, 20, 40, 80 min), then publish a
  command from the server. The first interval where it doesn't arrive brackets the timeout.
- **Session expiry** matters only across reconnects. As long as TCP stays up, eDRX doesn't
  disconnect MQTT: the broker sends straight away and the network buffers the downlink until the
  next paging window. esp-mqtt uses MQTT 3.1.1, where how long a persistent session lives is a
  broker setting, not something the device asks for (`MqttDriver::connect(startCleanSession)` only
  picks clean vs persistent). It needs to cover the longest reconnect gap we expect, which stays
  short until we add PSM in stage 6. Check the broker's setting then; no change needed for stages
  3–4.

### UART baud rate

The BC660K supports `AT+IPR` up to 460800, but 115200 (≈ 11.5 KB/s) is far above what NB-IoT
delivers: the manual's own `AT+QIPERF` example shows ~10–12 kbps uplink (≈ 1.5 KB/s). So no need
to go faster. The only remaining question is whether a *slower* rate saves meaningful power on
the UART, which isn't worth chasing before the stage 4 current measurements.

### Measuring bytes and airtime

- `AT+QENG=2` reports Tx/Rx time (radio-on) since the counters were last reset.
- `AT+QIPERF` gives a baseline throughput figure for the cell we're on.
- On the device, count bytes in `AtSocketTransport` per direction, and publish them in telemetry
  next to the link-quality fields.
- 1NCE's portal shows per-SIM data usage. Use it to cross-check the device-side counters.
