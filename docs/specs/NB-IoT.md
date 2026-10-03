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
   (`AT+QIOPEN` / `QISEND` / `QIRD`). `esp_mqtt_client_config_t.network.transport` accepts a
   custom transport (`nullptr` for WiFi builds in `MqttDriver::configMqttClient()`), so
   `MqttDriver` itself stays almost untouched, and the bytes on the wire are the same as over WiFi.

**We go with option 2.** None of the protocols is ours to implement:

| Layer | Library |
| ----- | ------- |
| MQTT | esp-mqtt (already used) |
| TLS | mbedTLS (already used) |
| AT framing, command/response matching, URC dispatch, generic 3GPP commands | [`espressif/esp_modem`](https://components.espressif.com/components/espressif/esp_modem), command mode |
| AT socket → `esp_transport` glue | Ours (`AtSocketTransport`), modelled on `tcp_transport_at.cpp` from esp_modem's [`modem_tcp_client`](https://github.com/espressif/esp-protocols/tree/master/components/esp_modem/examples/modem_tcp_client) example |
| TLS over that transport | Ours (`TlsTransport`), mbedTLS directly |
| BC660K-specific commands | Ours (`Bc660KDriver`) |

The `modem_tcp_client` example is exactly this setup: `tcp_transport_at.cpp` plugged into
esp-mqtt, a `sock_dce` layer, and per-chip `sock_commands_bg96.cpp` / `sock_commands_sim7600.cpp`.
The BG96 is a Quectel module using the same `QI*` socket commands as the BC660K. We kept the
shape (an `esp_transport` over the module's socket commands) but not the `sock_dce` layer: it
moves raw bytes, with a `>` prompt to wait for on send and binary data in the middle of the
`QIRD` response, which our line-based command path can't take. See "Socket data as hex" below.

Alternatives looked at:
[TeschRenan/BC660K-GL](https://github.com/TeschRenan/BC660K-GL) (MIT, ESP-IDF, BC660K-specific)
brings its own UART and AT layers, duplicating esp_modem, and has no esp-mqtt transport. Not a
dependency, but a useful second reference for BC660K response formats. The other BC660K libraries
are Arduino or Mbed.

TLS runs on the ESP32 with mbedTLS, layered over the AT socket. esp-tls can't do this (it opens
its own lwIP socket), and the example's `tls_transport.cpp` skips certificate verification, so
`TlsTransport` drives mbedTLS directly. That keeps certificate handling identical to the WiFi path
(`configServerCert`, verification required, hostname checked), keeps the TLS endpoint ours rather
than the modem firmware's, and is the same mbedTLS we will need for DTLS later. RAM shouldn't be
a problem: the WiFi path already runs MQTT over TLS, and a cellular build doesn't start the WiFi
driver, lwIP's WiFi netif or their buffers, so it should need less internal RAM, not more.

### Socket data as hex

The BC660K can take socket data as hex instead of raw bytes (`AT+QICFG="dataformat",1,1`). Then
`AT+QISEND=0,<len>,"<hex>"` and `+QIRD: <len>,<remaining>,"<hex>"` are ordinary text lines, so
sends and reads go through the same command path, response parser and URC handling as every
other command, and binary data never reaches the line parser. The cost is twice the bytes on the
UART and 1024 bytes per `QISEND` instead of 2048. Neither matters: 115200 baud is about 8× what
NB-IoT delivers, and the bytes over the air are the same.

### Leave room for other chipsets

```text
CellularDriver              ← owns the UART, power/wake, registration, networkReady, time, telemetry
  └─ CellularModuleDriver   ← chipset-specific AT: init sequence, sockets, URC parsing, NTP
       └─ Bc660KDriver      ← esp_modem GenericModule subclass; Quectel QI*/QENG/QSCLK commands
AtSocketTransport           ← esp_transport for esp-mqtt, talks only to CellularModuleDriver's socket API
TlsTransport                ← mbedTLS over any esp_transport; MqttDriver wraps AtSocketTransport in it
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

CMake turns the setting into one compile definition per link, `UD_CONNECTIVITY_WIFI` or
`UD_CONNECTIVITY_CELLULAR`. Code is gated on the link it needs (the WiFi driver, OTA and the
`wifi` telemetry section on `UD_CONNECTIVITY_WIFI`; the modem on `UD_CONNECTIVITY_CELLULAR`),
never on the other one being absent, so a `BOTH` build only has to define both.

Both drivers set the same `networkReady` state, so `MqttDriver` and the rest don't need to know
which link is in use. Each logs the details of its link (SSID or cell, and the IP address) when
the network comes up; the "Device ready" line no longer does, since with NB-IoT the device is
often ready before the network is. Later this becomes a `network-config` setting, and later
still, use whichever link is available.

### UART0 console fallback

USB Serial/JTAG is the default console (#640). `-DUD_UART0_CONSOLE=1` puts it back on UART0
for early-boot debugging. The cellular driver checks `CONFIG_ESP_CONSOLE_UART_NUM == 0` at
startup and then **does not claim GPIO16/17**: it logs a warning and the device runs offline.
That way one build can still be debugged over the pogo header, at the cost of NB-IoT.

### Link quality in telemetry

The cellular build adds a `cellular` section to device telemetry, next to where the WiFi fields
go: `cell`, `band`, `rsrp`, `rsrq`, `sinr` and `ecl` from `AT+QENG=0` (as of the last
registration check, while camped), plus `bytes-sent` / `bytes-received` over the modem since the
last telemetry message. The server ignores fields it doesn't
know, so this needs no server change up front; server-side handling follows once the shape has
settled.

### OTA is off in cellular builds until stage 5

`HttpUpdater` downloads with `esp_http_client` over lwIP and waits on `WiFiDriver` directly, so it
can't work over the modem. In a build without WiFi:

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

- [x] Add `espressif/esp_modem` (2.1, PPP off, URC handler on); builds with our C++ exceptions. Flash cost: +67 KB for the cellular build over a release WiFi build; WiFi builds don't link it
- [x] `-DUD_CONNECTIVITY=WIFI|CELLULAR` CMake setting; in this stage `CELLULAR` only adds the modem driver next to WiFi
- [x] Describe the modem UART pins per device model (`DeviceDefinition::getCellularModemPins()`). MK13: TX/RX are swapped (hardware#93), so `tx = GPIO17`, `rx = GPIO16`
- [x] Use UART1 through the GPIO matrix; don't start if the console is on UART0 (see above)
- [x] `CellularDriver`, `CellularModuleDriver` interface, `Bc660KDriver : GenericModule`
- [x] Wake: send `AT` repeatedly with backoff until `OK` (10 attempts, 100 ms → 1 s); the first `AT` is consumed by the deep-sleep wake. Every command goes through this sequence
- [x] Boot init, sent every boot: `ATE0`, `AT+CMEE=2`, `AT+QSCLK=0` (not persisted), `AT+CEREG=3`, `AT+QNBIOTEVENT=1,1`. DNS (`AT+QIDNSCFG`) moves to stage 3 with the sockets
- [x] Log the IMEI (`AT+CGSN=1`) and SIM state (`AT+CPIN?`) at startup; while searching, also log `AT+QENG=3` (EMM / PLMN state) and `AT+COPS?`
- [x] Read, and only write when they differ (both persist in NVRAM and are slow): `AT+CFUN=1` (at 0 the SIM isn't even powered) and `AT+QBAND=2,20,8` (EU bands; without a list the module scans every band). Both were steps in the bench bring-up sequence that the first driver lacked
- [x] Poll registration (every 30 s while searching, 5 min once registered) and log it with decoded signal and serving cell; on registering, log `+COPS`, `+CGATT` and `+CGDCONT`. `+CEREG` URCs only report changes, so a modem that keeps searching was silent
- [x] Shared quote-aware field tokenizer (`splitAtFields` / `AtField`) for `+XXX:` information lines, with decoders for `+CEREG`, `+CSQ` and `+QENG: 0`
- [x] URCs that arrive in pieces are handled exactly once: esp_modem only empties its receive buffer after a command, so the driver tracks how much of it it has already handled
- [x] Log `ATI` (includes the firmware revision, so no separate `AT+CGMR`), `AT+CIMI`, `AT+QCCID`, `AT+CSQ`, `AT+CEREG?`, `AT+QENG=0`; log `+CEREG` URCs decoded, other URCs raw
- [x] Hold a no-light-sleep PM lock while the modem is up: the UART driver only keeps the chip awake while transmitting, so replies and URCs would be lost in light sleep (stage 4 replaces this)
- [x] Unit-test the response parsers in `test/unit-tests/` (final result codes, echo, `+CME ERROR`, both `+CEREG` shapes, field tokenizing, `+CSQ`, `+QENG: 0` searching and camped)
- [x] Verify on a Desert Lark board (MK13, modem firmware `BC660KGLAAR01A05`, 1NCE SIM): the modem answers on the first wake, the settings apply, and every status command returns the documented shape, so the test fixtures stand
- [x] See the URC path work: `+CEREG` URCs logged for both a denial and the registration. On the bench MK13, registration took about 3 minutes from a cold start: cell `0014B307` (TAC 6216) repeatedly rejects with EMM cause 15, then the module registers on `0014B501` (Telekom `21630`, roaming, APN `SENSOR.NET`) at ECL 2 (RSRP −111 dBm, SINR −5 dB)

Follow-ups, not needed for stage 3:

- [ ] MK14: open-drain `RESET_N` on GPIO4, pulsed once after boot (once there is an MK14 device definition)
- [ ] Make the band list configurable before devices go outside Europe
- [ ] Bench aid: AT passthrough from the console (type an AT command, see the reply), debug builds only. Needs console input over USB Serial/JTAG, which nothing reads today

### Stage 3 — MQTT over NB-IoT (`UD_CONNECTIVITY=CELLULAR`)

Goal: BOOT, SYNC (config/update request) and TELEMETRY reach the existing broker over NB-IoT.

- [x] `initConnectivity()` builds `CellularDriver` instead of `WiFiDriver`; BLE WiFi provisioning hooks skipped (BLE can still set the time). Telemetry leaves out the `wifi` section. A cellular build on a board without a modem connector throws at startup instead of running offline for good
- [x] Registration state machine: a `+CEREG` URC triggers a fresh `AT+CEREG?` straight away, on top of the polling from stage 2. The two `+CEREG` shapes are parsed separately (the URC leads with `<stat>`, the read response with `<n>`), and the EMM reject cause is logged (no coverage vs subscription refused)
- [x] `networkConnecting` / `networkReady` set from registration + an address on PDP context 0 (`AT+CGPADDR=0`; the `+IP:` URC also triggers a look)
- [x] DNS: `AT+QIDNSCFG=0` after attach; fall back to public servers only when the network hands out none
- [x] `AtSocketTransport`, modelled on `tcp_transport_at.cpp`: `QIOPEN` with the hostname directly (context 0), `QISEND`, `QIRD` in **buffer access mode**, `+QIURC: "recv"` / `"closed"` URCs, and a `QIRD` every 10 s even without a URC, in case one was lost. Socket data as hex (see "Socket data as hex"). Logs bytes sent and received per connection
- [x] Commands whose outcome follows the `OK` (`SEND OK`, `+QIOPEN:`, `CLOSE OK`, `+QNTP:`) wait for that line as part of the command, since esp_modem drops whatever follows a completed command in the same read. URCs that arrive in the middle of a command's response go to the URC handler instead of being lost among its lines
- [x] Chunk writes at 1024 B (`QISEND` in hex mode) and reads at 512 B (`QIRD`)
- [x] mbedTLS over the AT transport (`TlsTransport`); same server cert, optional client cert, verification required. Logs how long the handshake took
- [x] `MqttDriver`: pass the custom transport in cellular builds; keepalive 10 min (pings every 5 min, about 58 KB/day) until the NAT timeout is measured, network timeout 30 s, 8 KB task stack for TLS on top of the AT layer
- [x] The transport stack follows the URI's scheme, since esp-mqtt ignores it once given a transport: TLS for `mqtts`/`wss`, IDF's WebSocket transport on top for `ws`/`wss`. WebSocket costs a few hundred bytes of HTTP upgrade per connection and 2–6 bytes per packet, so plain `mqtts` is the better choice for NB-IoT once the broker is reachable that way
- [x] OTA off: reject `firmware` entries with `Unimplemented`, don't register `http-update`
- [x] Time: `RtcDriver` takes time from the modem instead of SNTP. NITZ first, as `+CTZEU` (`AT+CTZR=3`), which carries UTC, so there's no offset to get wrong. `AT+QNTP` 30 s after the network is up if no NITZ came, then daily. (`AT+CTZU` is a BG96 command the BC660K doesn't have, and `+CCLK` would mean guessing whether the module reports local time)
- [x] Telemetry: link-quality fields from `AT+QENG=0`, and bytes sent and received over the modem (see "Link quality in telemetry")
- [x] `DebugConsole`: in cellular builds, show the cellular link in place of `WIFI: off` (registration state, IP address, RSRP/ECL). It refreshes every 250 ms, so it reads what `CellularDriver` last saw (`getStatus()`, updated on every registration check) rather than sending AT commands itself
- [x] Keep the modem awake (no PSM/eDRX) and the TCP+TLS session up for this stage, to separate "does it work" from "does it sleep"
- [x] Demo: BOOT, SYNC and TELEMETRY visible on the server from a Desert Lark board (MK13, 1NCE). With the modem kept awake, a valve override from the web app also arrives within seconds; stage 4 has to keep that working with eDRX
- [x] Measure, on the device: bytes per telemetry interval (`cellular` telemetry), connect time (`Connected to MQTT server in … ms`), TLS handshake time and bytes (`Handshake with … done in …`)
- [ ] Measure on the bench: bytes per hour and per message type, connect time, TLS handshake bytes (see open questions for how)

To check on the first bench run:

- [x] Hex mode applies to `QIRD` output in buffer access mode too: TLS and MQTT run over it
- [ ] `SEND OK` arrives after the `OK` as documented; what makes `SEND FAIL` happen, if anything does (the driver retries three times, 1 s apart)
- [x] 1NCE (Telekom `21630`, roaming) sends NITZ with the time on attach: `+CTZEU: "+8",1,"2026/10/02,23:50:20"` (for cornucopia-app#507). Note the time zone came as `+8`, not the zero-padded `+08` the manual describes
- [ ] Whether `+QNTP` reports UTC as the TCP/IP application note says (the raw line is logged at debug level); not exercised yet, since NITZ arrived
- [x] 1NCE hands out DNS servers with the PDP context (`8.8.8.8`, `8.8.4.4`)
- [x] TLS handshake over NB-IoT at ECL 0: about 3.8 s; from network ready to MQTT connected (with `wss`) about 6.8 s, well within the 30 s network timeout
- [x] Registration after the first bring-up is fast: about 5 s from boot on cell `0014B307`, which rejected the SIM with EMM cause 15 for minutes during stage 2. Likely the module's stored network state and band list; to tell apart with a full power cycle

### Stage 4 — Commands and updates, eDRX

Goal: commands and UPDATE messages sent from the server arrive with predictable latency, and
the modem sleeps between paging windows.

- [ ] Health check: MQTT keepalive / DNS lookup, **not** ping (ICMP is blocked on `SENSOR.NET`). Moved from stage 3: once the modem sleeps, "not answering" looks different
- [ ] Recovery: `AT+QRST` on a modem that stops responding; on MK14+ pulse `RESET_N` instead. On MK13 `AT+QRST` only helps while the modem still answers, and rebooting the ESP32 doesn't power-cycle the modem

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
  hex mode. We use hex mode (see "Socket data as hex"), so 1024.
- `AT+QIRD=<id>,<read_length>`: **1–512 bytes** per read.
- The modem buffers at most **2 KB** of received data per socket in buffer access mode. A large
  UPDATE has to be drained with several `QIRD`s as it arrives. We expect TCP flow control to hold
  back the rest rather than lose it; to be confirmed on the bench.
- `AT+QICFG="showlength",1` adds the remaining-length field to `QIRD` responses and the
  `recv` URC, so the transport knows how much more to read.

Steps:

1. Set the transport's write chunk to 1024 and its read chunk to 512 (done). esp-mqtt writes
   whole packets, up to `buffer.out_size` (4 KB today), so writes will be split.
2. Confirm on the bench: send increasing sizes to an echo server (256 B → 4 KB), checking for
   `ERROR` and comparing with `AT+QISEND=<id>,0`, which reports bytes sent / acked / unacked.
   Then have the server send a SYNC/UPDATE larger than 2 KB.

### Keepalive and session expiry

- **Keepalive must be shorter than the carrier's NAT idle timeout.** Otherwise the NAT drops the
  TCP mapping, the broker's downlink goes nowhere, and we only notice on the next uplink. 1NCE's
  value isn't known yet; carrier NAT timeouts vary from minutes to hours.
- **Keepalive is the biggest fixed data cost.** esp-mqtt sends a PINGREQ every *keepalive/2*
  whether or not other traffic went out (it only resets the timer on CONNACK and PINGRESP, see
  `process_keepalive()` in `mqtt_client.c`). The WiFi build's 120 s keepalive means a ping a
  minute. Each
  ping + response over TLS/TCP is roughly 200 B with ACKs (estimate: 2 B MQTT + 29 B TLS record
  + 40 B TCP/IP, each way, plus ACKs), so about **290 KB/day**. That alone is twice the 1NCE
  budget (500 MB / 10 years ≈ 137 KB/day). Cellular builds use 10 min for now (a ping every 5
  min, about 58 KB/day); at 30 min (a ping every 15 min), it's about 19 KB/day.
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
- On the device, `AtSocketTransport` counts bytes per direction: per connection in the log when
  it closes, and per telemetry interval in the `cellular` telemetry section. These are TLS
  records and everything inside them, but not TCP/IP headers, ACKs or retransmissions.
- 1NCE's portal shows per-SIM data usage. Use it to cross-check the device-side counters.
