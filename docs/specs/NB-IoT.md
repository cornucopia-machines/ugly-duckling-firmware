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
- **DRX** (Discontinuous Reception) — plain paging: an idle modem listens every 1.28–2.56 s.
- **PTW** (Paging Time Window) — how long the modem listens at the start of each eDRX cycle
  (2.56–40.96 s), before it goes back to sleep for the rest of the cycle.
- **RRC** (Radio Resource Control) — *connected* means the modem has a radio link to the cell
  and draws milliamps; *idle* means it only listens for paging and draws microamps. After any
  traffic it stays connected until an inactivity timer runs out (60 s by default on the BC660K).
- **RAI** (Release Assistance Indication) — a flag on a send telling the network to release the
  RRC connection right after it (or after the reply), instead of waiting for the inactivity timer.

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

### Sleeping: eDRX, not PSM

The device has to stay reachable for commands, so the modem uses eDRX and PSM stays off
(`AT+CPSMS=0`, checked on every boot). In eDRX the BC660K only ever *light*-sleeps between
paging windows (deep sleep is PSM-only, per the hardware design guide), which keeps the TCP
socket and the TLS/MQTT session on top of it. Light sleep at eDRX 40.96 s is 38 µA
(datasheet, ECL 0); PSM would be 0.8 µA, but the device would be unreachable between its own
wakes. That 37 µA isn't where the power goes (see "Power budget" below), so PSM only pays off for
devices that report a few times a day, and stays an option for stage 6.

The modem settings live in the `cellular` section of **device-config**, not network-config, so
changing them doesn't mean sending network-config's credentials to the device again:

```json
"cellular": {
  "edrxCycle": 0
}
```

- `edrxCycle` is in milliseconds, and has to be one of the cycles NB-IoT has (`AT+CEDRXS`):
  20480, 40960, 81920, 163840, 327680, 655360, 1310720, 2621440, 5242880 or 10485760. Anything
  else is logged and leaves the module's setting as it was. **The default is 0, eDRX off**,
  because the network we use doesn't grant it (see the bench notes under stage 4). Where a
  network does, 40.96 s would be the cycle to start with: commands arrive within about 45 s, it's
  the one cycle the datasheet gives a current for, and at 38 µA the modem is already well below
  the ESP32's light-sleep current, so longer cycles would make commands slower for little gain.
- With eDRX off, the modem listens every DRX cycle, the paging cycle the cell broadcasts (1.28,
  2.56, 5.12 or 10.24 s in NB-IoT; `AT+QDRX?` reads it), so commands arrive within seconds, at
  110–220 µA at the two shorter cycles (datasheet, ECL 0), and it still sleeps in between. That
  is also the most responsive setting; a short eDRX cycle wouldn't be, the shortest being
  20.48 s. `sleepWhenIdle: false` keeps the ESP32 itself awake on top of that.
- The paging time window is left to the network for now (`AT+QEDRXCFG` could request one).
- The network has the final say: the driver logs the granted cycle and PTW after registering
  (`AT+CEDRXRDP`) and whenever the network changes them (`+CEDRXP`).

### Waking the ESP32 on UART edges

Desert Lark doesn't wire the modem's RI or PSM_EINT pins (by design, to save GPIOs; see the
hardware spec), so the only sign of the modem having something to say is activity on its TX
line, our UART RX. The ESP32-C6 can wake from light sleep on UART RX in four ways:

| Mode | Wakes on | Clock in sleep | Bytes lost |
| ---- | -------- | -------------- | ---------- |
| Edge count (`UART_WK_MODE_ACTIVE_THRESH`) | ≥ 3 RX edges | none | the first few (≈ 318 µs wake-up ≈ 4 bytes at 115200) |
| FIFO threshold / start bit / character sequence | received data | 40 MHz XTAL | none |

Keeping the 40 MHz crystal on in light sleep costs about 3.3 mA instead of about 34 µA
(Espressif's BLE power-save example, ESP32-C6), a hundred times more than the modem itself, so
**we use edge counting and accept losing the start of whatever woke us.** None of it matters,
because every URC has a query for the same state:

| URC | Re-read with |
| --- | ------------ |
| `+QIURC: "recv"` | `AT+QIRD`: the data waits in the module's buffer (buffer access mode) |
| `+QIURC: "closed"` | `AT+QIRD` fails |
| `+CEREG` | `AT+CEREG?` |
| `+IP` | `AT+CGPADDR` |
| `+CSCON` | `AT+CSCON?` |
| `+CEDRXP` | `AT+CEDRXRDP` |
| `+CTZEU` | NTP fallback after 30 s |

So a line that doesn't parse as a known URC means "something happened": poll the socket and
re-read the registration. RI would have avoided the lost bytes, but buys nothing on top of this.

### Power budget

Today's 30–40 mA is mostly the ESP32: `CellularDriver` holds a no-light-sleep lock for as long
as the modem is up, in every cellular build.

Measured on the bench (MK13, `UD_NOSLEEP` debug build, battery current from the BQ27220):
**RRC connected costs about 10 mA more than RRC idle** (29 mA vs 19 mA, with the module's own
sleep still off, `AT+QSCLK=0`). The datasheet has no figure for this state; the estimates below
had assumed 3–5 mA.

How long the radio stays connected after a transfer (the RRC tail) is still to be measured. After
MQTT traffic it went idle about 50 s after what looked like the last transmission, but after a
cell change, with only signalling, 20–25 s later. That suggests the network releases after about
20 s, short of the module's own 60 s (`AT+QCFG="DataInactTimer"`), and that something kept sending
after the MQTT traffic (in a debug build, published log records are the likely candidate). To
measure with `publishLogs` at `Info`: the Verbose `RRC connected` / `RRC idle` log lines against
the last `Queuing topic …`, and `rrc-idle-ratio` / `rrc-connections` per telemetry interval.

Estimates for a release build with stage 4 done, eDRX off (see stage 4):

| Part | Average |
| ---- | ------- |
| ESP32-C6 light sleep, peripherals powered (needed for UART wake) | ~0.2 mA, plus wakes for timers and tasks |
| Modem sleeping between pages: 0.22 mA at a 1.28 s paging cycle, 0.11 mA at 2.56 s (datasheet, ECL 0); less at 5.12 or 10.24 s | 0.03–0.22 mA |
| Rest of the board | to measure (sleep floor of a WiFi release build) |
| Telemetry every 5 min: ~1.5 s Tx at ~100 mA, then a 20–50 s RRC tail at ~10 mA | 1.2–2.2 mA |
| Keepalive ping every 5 min (esp-mqtt pings even after a publish), the same | 1.2–2.2 mA |

That is **about 2.5–5 mA** with the defaults, depending on the tail, and the tail after each
transfer is most of it. Releasing RRC right after a transfer (RAI on `QISEND`; a shorter
`AT+QCFG="DataInactTimer"` only helps if it's shorter than the network's own timer) and a
keepalive as long as the NAT allows would bring it to **about 1–1.5 mA**. The floor, set by sleep, is **about 0.25–0.45 mA plus the board**; eDRX at
40.96 s would only have saved up to 0.2 mA of that. The order of wins: ESP32 light sleep, then
the RRC tail, then fewer transmissions. The `rrc-idle-ratio` and `rrc-connections` telemetry fields
(share of time spent RRC idle and number of RRC connections since the last telemetry message)
track the tail.

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

- [x] `cellular` section in device-config (see "Sleeping: eDRX, not PSM")
- [x] PSM off (`AT+CPSMS=0`), only written when it differs
- [x] eDRX with the configured cycle (`AT+CEDRXS=2,5,…`), or off (`AT+CEDRXS=3`), written on every boot: `AT+CEDRXS?` shows the requested cycle whether or not eDRX is on, so it can't tell whether a write is needed. Log what the network grants (`AT+CEDRXRDP` on every registration check, logged when it changes; `+CEDRXP`)
- [x] Idle paging cycle (`AT+QDRX?`, only answers with it while RRC idle), logged when it changes: without eDRX, that is what bounds command latency and the modem's sleep current
- [x] RRC state (`AT+CSCON=1`, `+CSCON` URC, re-read on every registration check): `CONN` / `IDLE` in the debug console, in telemetry the share of time spent RRC idle and the number of RRC connections since the last telemetry message (`rrc-idle-ratio`, `rrc-connections`, like `pm.sleep-ratio` / `pm.sleep-count`). Changes are only logged at Verbose: debug builds published every log line over MQTT, so logging "idle" brought the radio straight back to connected, and the bench device never left `CONN`. Any steady stream of published logs keeps RRC connected the same way, so measure power with `publishLogs` at `Info` (the release default). Debug builds now default to `Debug` instead of `Verbose`
- [x] Modem sleep: `AT+QSCLK=2` (light sleep only; `1` would also allow deep sleep, which only happens in PSM). `AT+QCFG="wakeupRXD",1` if it's off, followed by `AT+QRST=1`, since it only takes effect after a restart
- [x] ESP32 light sleep: wake on UART RX edges (see "Waking the ESP32 on UART edges"); hold the no-light-sleep lock only while a command is in flight; any line that doesn't parse as a URC means "poll `QIRD` and re-read registration and RRC state" (logged at Verbose only, since a published log record would wake the radio)
- [x] The transport's safety `QIRD` poll goes from 10 s to 2 min: the module stays awake for 10 s after any UART activity (`AT+QCFG="slplocktimes"`), so a 10 s poll would keep it awake for good. Data found by this poll is logged (`Found … bytes the modem didn't announce`), to tell how often URCs get lost altogether
- [ ] Revisit whether the safety poll is needed at all: if the bench never logs `Found … bytes the modem didn't announce` (every wake leaves at least part of a line, which already triggers a read), drop it, and with it the module wake-ups it costs; esp-mqtt's keepalive still catches a connection that went quiet
- [ ] Keepalive in device-config (`cellular.keepalive`); measure 1NCE's NAT idle timeout and set the default from it (see "Keepalive and session expiry")
- [ ] Subscriptions over the AT transport: commands (QoS 2) and `update` (QoS 1) arrive while the modem sleeps
- [ ] Measure command latency against the paging cycle, average current (`current` in telemetry), `rrc-idle-ratio` and `rrc-connections`
- [ ] Demo: valve override from the app reaches the device over NB-IoT

To check on the bench:

- [x] PSM was on: the bench module answered `+CPSMS: 1,,,"01000001","00000101"` (PSM requested,
  periodic TAU 10 h, active time 10 s), presumably since before stage 2. The driver now turns it
  off; the change starts a tracking area update (`AT+QENG=3` shows `"TAU INIT"`)
- [ ] What Telekom (`21630`) grants for eDRX, and the PTW. Right after registering on cell
  `0014B307`, during that TAU, `AT+CEDRXRDP` said the cell doesn't use eDRX; since the grant comes
  with an attach or TAU, the driver now re-reads it on every registration check and logs changes.
  It stayed that way, but the driver never wrote `AT+CEDRXS`: the read already answered
  `+CEDRXS: 5,"0011"` (the manual's own example, so probably the factory default) without eDRX
  being on. Now written on every boot. With `AT+CEDRXS=2,5,"0011"` sent before the attach, so
  that the attach itself carries the request, `AT+CEDRXRDP` still answers `+CEDRXRDP: 0` ("access
  technology not supporting eDRX") on that cell, and no `+CEDRXP` follows: so far it looks like
  Telekom doesn't grant eDRX to 1NCE's roaming SIMs there
- [ ] What the lines that woke the ESP32 look like after losing their first bytes (Verbose `Unrecognized line from the modem`), and whether any get lost altogether (`Found … bytes the modem didn't announce`)
- [ ] Whether the module had `wakeupRXD` on, and how long `AT+QRST=1` takes if not
- [ ] ESP32 `pm.sleep-ratio` and battery current in a sleeping build, with the radio idle
- [x] The first sleeping build froze the first time the ESP32 went to light sleep: no log, no
  watchdog reset, no core dump. Without the UART wake-up it still froze; with IDF's UART handling
  before sleep turned off for UART1 (`ESP_SLEEP_NO_HANDLING`) it didn't. Before light sleep IDF
  suspends every enabled UART (`sleep_uart_prepare()`), forcing XOFF and waiting for the UART to
  sync its registers, which needs the UART's clock. WiFi builds never hit this: since the console
  moved to USB Serial/JTAG they have no HP UART enabled. The modem UART now runs on XTAL instead
  of the default PLL clock
- [x] Confirmed: with the modem UART on XTAL (and IDF's UART handling before sleep left on), the
  sleeping build boots, connects and stays connected
- [x] esp_modem logs `unknown uart event type: 8` (`UART_WAKEUP`, which it doesn't handle) as a
  warning on every UART wake. Published, each one was an uplink whose acknowledgement woke the
  ESP32 again: the server showed lost log records and a subscription timeout. The
  `uart_terminal` tag now logs errors only
- [x] The MQTT connection attempt and subscription acks were still bounded by the WiFi network
  timeout (15 s) over NB-IoT; they now use the modem's 30 s too. A subscription that times out
  is only made again with the next clean session, so commands stopped arriving; recovering from
  that is #685
- [x] The radio never goes RRC idle with the MQTT connection open to `mosquitto-home` (Tailscale
  Funnel), even with no MQTT traffic for minutes. Suspected cause: Funnel's TCP keepalives (see
  "Keepalive and session expiry"). To confirm: the Verbose `RRC connected` / `RRC idle` lines
  (alternating about every 15 s plus the RRC tail means keepalive probes), and a run against a
  broker that isn't behind Funnel
- [x] Confirmed: against the staging broker (not behind Funnel) the radio goes RRC idle, and
  battery current drops from about 29 mA to 19 mA
- [ ] The RRC tail after a transfer (see "Power budget"): about 50 s after MQTT traffic in a debug
  build, 20–25 s after a cell change

Moved out of stage 4:

- QoS 1 redelivery after a reconnect: `MqttDriver` always starts a clean session, so the broker
  drops anything queued while the device was away. Persistent sessions are #682
- Health check and recovery from a modem that stops answering (`RESET_N` on MK14+; MK13 can't
  reset the modem from firmware): #683

### Stage 5 — OTA over NB-IoT

- [ ] Decide the download path: HTTP(S) over the AT socket transport (esp_http_client has no custom transport hook, so it needs a thin HTTP client or the localhost-listener trick from the esp_modem example) vs the modem's HTTP commands, if the BC660K has them
- [ ] Resume interrupted downloads (HTTP range requests); a ~1.5 MB image is a large share of the 1NCE data budget, so retries must not start from zero
- [ ] Ship firmware update over NB-IoT end-to-end, including rollback; remove the stage 3 rejection
- [ ] Separately: modem firmware updates (Quectel DFOTA), if we need them

### Stage 6 — Use less data and battery, get lower latency

Order to be decided from the stage 3–4 measurements.

- [ ] TLS 1.3 PSK session resumption for MQTT (cheap stopgap from cornucopia-app#492)
- [ ] PSM between wakes for nodes that can tolerate command latency, with a cellular-specific wake/sync cadence (`AT+QSCLK=1` then, for deep sleep)
- [ ] Release the RRC connection sooner after a send: RAI on `QISEND` (`rai=2`, after the reply) or a shorter `AT+QCFG="DataInactTimer"` (default 60 s); see "Power budget"
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
  minute. Each ping + response over TLS/TCP is roughly 200 B with ACKs (estimate: 2 B MQTT,
  29 B TLS record and 40 B TCP/IP, each way, plus ACKs), so about **290 KB/day**. That alone is twice the 1NCE
  budget (500 MB / 10 years ≈ 137 KB/day). Cellular builds use 10 min for now (a ping every 5
  min, about 58 KB/day); at 30 min (a ping every 15 min), it's about 19 KB/day.
- **Server-side TCP keepalives hide the NAT timeout, and keep the radio connected.** Tailscale
  Funnel (the `mosquitto-home` bench broker) accepts connections on Go servers, and Go enables
  TCP keepalives by default: a probe after 15 s idle, then every 15 s. Each probe is a downlink
  packet: it keeps the carrier NAT mapping alive, pages the modem, and restarts its 60 s RRC
  inactivity timer, without any traffic MQTT or the device's byte counters would show. So measure
  the NAT timeout, and RRC idle, through a broker endpoint without TCP keepalives, and check the
  production endpoint for them too.
- **So: keepalive ≈ 2 × (NAT timeout − margin)** (pings go out at half the keepalive), capped by
  how quickly we want to notice a dead link. Measure the NAT timeout: open an MQTT session with a
  very long keepalive, stay idle for increasing periods (5, 10, 20, 40, 80 min), then publish a
  command from the server. The first interval where it doesn't arrive brackets the timeout.
- **Keepalive vs eDRX cycle.** They cost energy in different ways, so they don't trade off
  one-for-one. A ping is an uplink: the modem sends it straight away, without waiting for a
  paging window, and then stays RRC connected until the inactivity timer runs out (60 s), at
  milliamps. So each ping costs a fixed amount of energy (a few seconds of Tx plus the 60 s
  tail, see "Power budget"), whatever the eDRX cycle. The eDRX cycle sets the *idle* current
  between transfers (38 µA at 40.96 s) and how long a command takes to arrive. Both add to the
  average; with pings every 5 min the pings dominate by far, which is why the keepalive should go
  as high as the NAT allows, and why releasing RRC early (RAI) matters more than a longer cycle.
- **Avoiding pings altogether: 1NCE's VPN Service** (not implemented, an option for later). 1NCE
  can put a SIM's traffic in an OpenVPN tunnel with a static private IP per SIM and no carrier
  NAT in between. With no NAT mapping to keep alive, the keepalive only has to detect a dead link,
  so it could go to hours. It needs our own OpenVPN endpoint in front of the broker, and the broker
  reachable only through it. First step: check in the 1NCE portal whether the SIM's IP address
  matches the `10.0.0.2` the PDP context reports (`AT+CGPADDR`); if it does, the address is
  already the SIM's own and the tunnel only has to route it.
- **Session expiry** matters only across reconnects. As long as TCP stays up, eDRX doesn't
  disconnect MQTT: the broker sends straight away and the network buffers the downlink until the
  next paging window. esp-mqtt uses MQTT 3.1.1, where how long a persistent session lives is a
  broker setting, not something the device asks for (`MqttDriver::connect(startCleanSession)` only
  picks clean vs persistent). It needs to cover the longest reconnect gap we expect, which stays
  short until we add PSM in stage 6. Check the broker's setting then. Today the device always
  starts a clean session anyway, so nothing queued while it was away is redelivered (#682).

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
