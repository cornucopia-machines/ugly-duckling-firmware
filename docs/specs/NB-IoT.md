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
- [cornucopia-app#533](https://github.com/cornucopia-machines/cornucopia-app/issues/533) — choose WiFi or cellular when (re-)provisioning a device
- [cornucopia-app#535](https://github.com/cornucopia-machines/cornucopia-app/issues/535) — firmware over MQTT in chunks, towards delta OTA ([#674](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/674))
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
   custom transport (`nullptr` over WiFi in `MqttDriver::configMqttClient()`), so
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
a problem: the WiFi path already runs MQTT over TLS, and a device on the cellular link doesn't start the WiFi
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
TlsTransport                ← mbedTLS over any esp_transport; MqttDriver and HttpUpdater wrap AtSocketTransport in it
```

Supporting another modem then means adding another `CellularModuleDriver`, the same way the
example adds `sock_commands_*.cpp` per chip.

### Choosing WiFi or NB-IoT

Every Carrot build has both links; `links` in **network-config** chooses one at boot:

```json
"links": ["cellular"],
"cellular": {
  "edrxCycle": 0
}
```

- `links` lists the links to use, in order of preference: `"wifi"` or `"cellular"`. Without it
  the device uses WiFi, as every device did before the setting existed. A list leaves room for
  "WiFi, falling back to NB-IoT" (`["wifi", "cellular"]`, stage 6) without changing the shape;
  until then only a single entry is supported.
- `cellular` holds the modem settings (see "Sleeping: eDRX, not PSM"). It's only used with the
  cellular link.
- Anything the device can't honor (more than one link, an unknown one, or `cellular` on a board
  without a modem connector or on Spinach) is logged as an error, and the device uses WiFi.
- Changing the link reboots the device, like any other network-config change. Only the driver
  for the chosen link is started: a device on the cellular link never starts the WiFi driver or
  lwIP's WiFi netif.
- Network-config carries the device's client key, which the server only keeps until the device
  confirms the config. So every change to `links` or `cellular` comes with a freshly issued
  client certificate and key. That's fine: these settings aren't meant to change in normal
  operation.
- The cellular code (esp_modem and the drivers) is only compiled on Carrot (`UD_PLATFORM_CARROT`,
  defined by CMake next to `UD_PLATFORM`), since the Desert Lark board only exists there.

Both drivers set the same `networkReady` state, so `MqttDriver` and the rest don't need to know
which link is in use. Each logs the details of its link (SSID or cell, and the IP address) when
the network comes up; the "Device ready" line no longer does, since with NB-IoT the device is
often ready before the network is. The BOOT message reports the link in use (`connectivity`).

A link change is confirmed as soon as the device boots with it, like any other configuration, so
switching a device without coverage or a working SIM to `cellular` leaves it unreachable until
someone gets to it. The next step is to confirm a link change only once MQTT has
connected over the new link, and revert otherwise (stage 4).

### UART0 console fallback

USB Serial/JTAG is the default console (#640). `-DUD_UART0_CONSOLE=1` puts it back on UART0
for early-boot debugging. The cellular driver checks `CONFIG_ESP_CONSOLE_UART_NUM == 0` at
startup and then **does not claim GPIO16/17**: it logs a warning and the device runs offline.
That way one build can still be debugged over the pogo header, at the cost of NB-IoT.

### Link quality in telemetry

On the cellular link, device telemetry has a `cellular` section, next to where the WiFi fields
go: `cell`, `band`, `rsrp`, `rsrq`, `sinr` and `ecl` from `AT+QENG=0` (as of the last
registration check, while camped), plus `bytes-sent` / `bytes-received` over the modem since the
last telemetry message. The server ignores fields it doesn't
know, so this needs no server change up front; server-side handling follows once the shape has
settled.

### OTA over the modem, without MQTT

Firmware updates use the same `HttpUpdater` on both links: the `firmware` entry in UPDATE (or the
`update` command) stores the URL and reboots, and the next boot downloads the image with
`esp_https_ota`, then reboots again either way. The BC660K has no HTTP AT commands
(`AT+QFOTADL` only updates the modem itself), so on the cellular link the HTTP client runs over
the modem's socket transport instead of lwIP: IDF's `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT`
lets `esp_http_client` take a transport of ours, which it uses for every connect, redirects
included. That transport is `AtSocketTransport`, with `TlsTransport` on top for HTTPS, verifying
the image host against the CA bundle (the same one the WiFi path uses) rather than the pinned MQTT
server certificate: `TlsTransport` takes `std::nullopt` for its credentials then. The HTTP client
and HTTPS OTA post their progress to IDF's default event loop, which used to be created by
`WiFiDriver`; `startDevice()` now creates it on every link.

The modem driver has a single socket, so on the cellular link the update boot downloads before
MQTT is started, and doesn't start it at all: there are no MQTT logs during the update, only the
serial console. The outcome reaches the server after the reboot, in BOOT and SYNC, as over WiFi.
Waiting for the network allows 5 minutes instead of 15 s, since registering can take that long,
and HTTP reads wait up to 30 s, like MQTT's over the modem.

A download that breaks off is resumed in the same boot with a Range request, which matters over
the modem: its receive buffer overflows now and then, and the TLS record that lost data fails
(see "`QISEND` / `QIRD` size limits"). Across a reboot it still starts over, and every update
costs about 2 MB of the 1NCE budget. See stage 5 for what comes next.

### Sleeping: eDRX, not PSM

The device has to stay reachable for commands, so the modem uses eDRX and PSM stays off
(`AT+CPSMS=0`, checked on every boot). In eDRX the BC660K only ever *light*-sleeps between
paging windows (deep sleep is PSM-only, per the hardware design guide), which keeps the TCP
socket and the TLS/MQTT session on top of it. Light sleep at eDRX 40.96 s is 38 µA
(datasheet, ECL 0); PSM would be 0.8 µA, but the device would be unreachable between its own
wakes. That 37 µA isn't where the power goes (see "Power budget" below), so PSM only pays off for
devices that report a few times a day, and stays an option for stage 6.

The modem settings live in the `cellular` section of **network-config**, next to `links` (see
"Choosing WiFi or NB-IoT"):

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
| Edge count (`UART_WK_MODE_ACTIVE_THRESH`) | ≥ 3 RX edges | none | the first few (≈ 318 µs wake-up ≈ 15 bytes at 460800) |
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
as the modem is up, on every device on the cellular link.

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

- [x] Add `espressif/esp_modem` (2.1, PPP off, URC handler on); builds with our C++ exceptions. Flash cost: +67 KB over a release WiFi-only build; every Carrot build links it since both links are built in (stage 4)
- [x] `-DUD_CONNECTIVITY=WIFI|CELLULAR` CMake setting; in this stage `CELLULAR` only adds the modem driver next to WiFi (replaced by `links` in network-config in stage 4)
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
- [ ] Wait long enough for the module to boot: on MK13 its `RESET_N` is tied to the ESP32's `RESET#`, so the reset button (or a brownout) restarts the module too, and the 10 wake attempts ran out before it answered ("is the daughter board connected?"); it answered a few seconds later
- [ ] Make the band list configurable before devices go outside Europe
- [ ] Bench aid: AT passthrough from the console (type an AT command, see the reply), debug builds only. Needs console input over USB Serial/JTAG, which nothing reads today

### Stage 3 — MQTT over NB-IoT

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
- [x] OTA off: reject `firmware` entries with `Unimplemented`, don't register `http-update` (until stage 5)
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

- [x] `cellular` section in device-config (see "Sleeping: eDRX, not PSM"); moved to network-config with `links`
- [x] `links` in network-config chooses WiFi or NB-IoT at boot, in place of `-DUD_CONNECTIVITY`; every Carrot build has both links, the cellular code gated on `UD_PLATFORM_CARROT` (see "Choosing WiFi or NB-IoT"). Links that can't be honored fall back to WiFi with an error; a firmware update pending from before a switch to cellular was dropped and rejected (until stage 5, which downloads it over the modem)
- [ ] Confirm a link change only once MQTT has connected over the new link; revert to the confirmed config otherwise. Firmware updates need the same delayed confirmation
- [ ] cornucopia-app: choose the link when (re-)provisioning a device, issuing a fresh client certificate and key with every change ([cornucopia-app#533](https://github.com/cornucopia-machines/cornucopia-app/issues/533))
- [x] PSM off (`AT+CPSMS=0`), only written when it differs
- [x] eDRX with the configured cycle (`AT+CEDRXS=2,5,…`), or off (`AT+CEDRXS=3`), written on every boot: `AT+CEDRXS?` shows the requested cycle whether or not eDRX is on, so it can't tell whether a write is needed. Log what the network grants (`AT+CEDRXRDP` on every registration check, logged when it changes; `+CEDRXP`)
- [x] Idle paging cycle (`AT+QDRX?`, only answers with it while RRC idle), logged when it changes: without eDRX, that is what bounds command latency and the modem's sleep current
- [x] RRC state (`AT+CSCON=1`, `+CSCON` URC, re-read on every registration check): `CONN` / `IDLE` in the debug console, in telemetry the share of time spent RRC idle and the number of RRC connections since the last telemetry message (`rrc-idle-ratio`, `rrc-connections`, like `pm.sleep-ratio` / `pm.sleep-count`). Changes are only logged at Verbose: debug builds published every log line over MQTT, so logging "idle" brought the radio straight back to connected, and the bench device never left `CONN`. Any steady stream of published logs keeps RRC connected the same way, so measure power with `publishLogs` at `Info` (the release default). Debug builds now default to `Debug` instead of `Verbose`
- [x] Modem sleep: `AT+QSCLK=2` (light sleep only; `1` would also allow deep sleep, which only happens in PSM). `AT+QCFG="wakeupRXD",1` if it's off, followed by `AT+QRST=1`, since it only takes effect after a restart
- [x] ESP32 light sleep: wake on UART RX edges (see "Waking the ESP32 on UART edges"); hold the no-light-sleep lock only while a command is in flight; any line that doesn't parse as a URC means "poll `QIRD` and re-read registration and RRC state" (logged at Verbose only, since a published log record would wake the radio)
- [x] The transport's safety `QIRD` poll goes from 10 s to 2 min: the module stays awake for 10 s after any UART activity (`AT+QCFG="slplocktimes"`), so a 10 s poll would keep it awake for good. Data found by this poll is logged (`Found … bytes the modem didn't announce`), to tell how often URCs get lost altogether
- [ ] Revisit whether the safety poll is needed at all: if the bench never logs `Found … bytes the modem didn't announce` (every wake leaves at least part of a line, which already triggers a read), drop it, and with it the module wake-ups it costs; esp-mqtt's keepalive still catches a connection that went quiet
- [ ] Keepalive in network-config (`cellular.keepalive`); measure 1NCE's NAT idle timeout and set the default from it (see "Keepalive and session expiry")
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

Goal: the existing HTTP update, with a full image, works over the modem, with firmware changes
only (see "OTA over the modem, without MQTT").

- [x] Turn on `CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT`
- [x] `TlsTransport`: without credentials (`std::nullopt`), verify against the CA bundle (`esp_crt_bundle_attach()`), with no client certificate, for the image host; with credentials, the pinned MQTT server certificate as before
- [x] Create the default event loop on every link, not only in `WiFiDriver`: without it, every event the HTTP client and HTTPS OTA post fails with `ESP_ERR_INVALID_STATE`, one error log line each
- [x] Hold the CPU at full speed, out of light sleep, for the whole update, so received data is read and decoded without delay. Also tried: skipping the wake-up `AT` before a command when the module had answered less than 5 s before, to save a round trip per `QIRD`. It didn't make downloads faster (the network sets the rate), and right after registering the module ignored a command sent less than a second after its last answer, so it was dropped again
- [x] Resume a download that broke off, in the same boot: a fresh connection asks for the rest with a Range request (`esp_https_ota`'s `ota_resumption`, from the bytes already written). A record whose MAC fails never reaches flash, so everything written is good. Gives up after 3 attempts in a row that got no further
- [x] Handle what arrives after a command's final result code in the same read: esp_modem empties its buffer when the command completes, so a `+QIURC: "recv"` right after an `AT+QIRD`'s `OK` was lost, and nothing read the socket until the module reported its buffer full. `parseAtResponse` reports where the response ends, and the rest goes to the URC handler; a line cut off at the end of the read counts as unrecognized, which makes the socket check for data anyway
- [x] Retry connecting too, not only a download that broke off, as long as no HTTP response came back: the TLS handshake lost data the same way once (the server's ~3 KB first flight didn't fit the buffer we weren't reading), and once the connection was closed ("by the peer or the network", the module can't tell) 4 s after the request went out. A server that answers with an error status isn't retried
- [x] Poll the module while data is flowing: every 250 ms (50 ms since the UART runs at 460800, see below) for 5 s after anything was sent or received, on top of the URCs. During downloads the module didn't announce data until its buffer was full: every `bytes waiting` line said `data announced ~120 ms ago` after 3–16 s without a read, and a handshake that received 2742 bytes got 2 announcements. The BG96, with the same `QI*` commands, documents that buffer access mode only announces again once a read has found the buffer empty; polling does that too. Outside that window, URCs and the 2-minute safety poll as before, so an idle MQTT connection doesn't keep the module awake
- [x] Drain the module's buffer: after a read that returned data, read again until one comes back `+QIRD: 0`, even when the remaining length already said 0. Going by the BG96, the module announces new data only once a read has found its buffer empty, so stopping at the last byte left the next data unannounced until the buffer filled. Costs one empty `QIRD` per burst; the polling above stays as a fallback until the bench shows announcements arrive promptly
- [x] Keep the module out of sleep (`AT+QSCLK=0` instead of `=2`) on a boot that downloads an update, as the AT manual recommends for data communication (`AT+QSCLK`, note 3: "Before data communication, it is recommended to execute AT+QSCLK=0"). It didn't make the module announce data any sooner (`not announced since` as before), and its buffer still overflowed: more than 2.6 KB arrived between two reads 250 ms apart. Polling back to back (`ACTIVE_POLL_INTERVAL` 0) didn't prevent that either; 250 ms is kept
- [x] First complete update over NB-IoT, at ECL 0 (RSRP -92 dBm, SINR 9 dB): 2.1 MB in about 17 minutes, with 3 resumes. Each break followed a `+QIURC: "recv",0,"buff full"` within about a second, while `2168 bytes waiting` without that URC didn't lose data. So the URC, not a full buffer, marks the loss. At ECL 1 with SINR 1–2 dB, downloads still broke off too often to finish
- [ ] Poll every 50 ms instead of 250 ms while data flows: at 460800 baud, every `bytes waiting` line still said `last read 249 ms ago`, i.e. more than 2 KB arrived between two polls, and the download ran at the same speed (the network sets it). To check on the bench: whether `last read` drops to ~50 ms, and whether `buff full` goes away
- [x] Log per connection, when it closes, how many of the bytes received were found after an announcement, by active polling, or by the safety poll (the reads draining a burst count towards whatever found it), plus the `buff full` URCs and UART errors. Data only polling found would otherwise have waited for `buff full`, so this tells whether polling is still needed. The `bytes waiting` line is verbose now: the buffer reached 2168 bytes all the time, often within 50 ms of the last read, without losing data
- [ ] UART overflows at 460800: one download broke off on an `AT+QIRD` that came back `OK` without its data line. esp_modem flushes the UART's buffers on a FIFO or ring buffer overflow, which takes the rest of the response with it, and logs it under `uart_terminal`, which we turn down to ERROR (for its light-sleep wakeup warnings); so these are now counted and logged as `UART error from the modem`. At 460800 the 128-byte hardware FIFO fills in under 3 ms. If they show up: a larger `rx_buffer_size`, the UART ISR in IRAM (`CONFIG_UART_ISR_IN_IRAM`, so flash writes don't hold it off), or a higher UART task priority
- [ ] Receive socket data as raw bytes instead of hex (`AT+QICFG="dataformat",1,0`, sending stays hex): about half the UART time per `QIRD`, so the buffer has more room before it overflows. The `QIRD` response then carries the bytes between quotes, `+QIRD: <n>,<remaining>,"<data>"`, so it has to be parsed by length, not by line; and whether the module passes every byte value through unchanged has to be checked on the bench
- [x] The UART at 460800 baud (`AT+IPR`), about four times less time per `QIRD` ([#690](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/690), [#694](https://github.com/cornucopia-machines/ugly-duckling-firmware/pull/694)); see "UART baud rate". Its effect on downloads is still to be measured
- [ ] Cat NB2 (release 14), once reading keeps up ([#691](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/691))
- [x] `HttpUpdater` waits on a `networkReady` state and takes an optional modem transport, instead of `WiFiDriver`; longer timeouts on cellular, where registration can take minutes
- [x] On the update boot over cellular, don't start MQTT: the modem driver has one socket (`CONNECT_ID = 0`) and the download needs it. No MQTT logs during the update; the outcome is reported in BOOT on the next boot, as over WiFi
- [x] Remove the stage 3 rejection: `firmwareUpdatesSupported`, `HttpUpdater::discardPendingUpdate()`, registering `http-update` on WiFi only
- [ ] Bench: a full image at ECL 0 and ECL 2, time and bytes (cross-check with the 1NCE portal); whether the modem's 2 KB receive buffer holds up under a sustained download
- [ ] Ship end-to-end, including rollback
- [ ] Separately: modem firmware updates (Quectel DFOTA), if we need them

To check on the bench:

- [x] The download works over the modem (MK13, 1NCE, RSRP −99 dBm, ECL 1): TLS handshake with
  `firmware.cornucopia-machines.eu` in about 5.2 s (275 bytes sent, 3 KB received), then a steady
  2 KB/s or so (`Downloaded 129.00 KB` 64 s after connecting), so roughly 17 minutes for a 2 MB
  image. Every `QIRD` took two round trips (the wake-up `AT`, then the read) for 512 bytes,
  which looked like the limit at first
- [x] The first full attempt failed after 769 KB, 414 s in: `Modem receive buffer full`, then
  `Verification of the message MAC failed` 7 s later, and the update was rejected (see
  "`QISEND` / `QIRD` size limits")
- [x] Skipping the wake-up `AT` and holding the CPU at full speed didn't change the rate: about
  2 KB/s again (RSRP −98 dBm, ECL 0), the same as at ECL 1, so the network sets it, not our reads.
  It failed the same way after about 1 MB, 531 s in. Both failures came 7–9 minutes into a
  steady download, which suggested something holding the module for a while: commands that
  waited over 1 s for another one, or took over 1 s themselves, are now logged at Debug, and so
  is the receive buffer filling past 1 KB
- [x] The UE category is Cat NB1 (`relversion` 13, `NBcategory` 1), which fits about 16 kbit/s
  (680-bit downlink transport blocks, 25.5 kbit/s peak). Cat NB2 (release 14) allows up to
  127 kbit/s if the network supports it (#691)
- [x] Nothing holds the module: no command waited or took over 1 s. Instead the receive buffer
  is regularly full during the download, `2168 bytes waiting` several times a minute from the
  first seconds on (so the "2 KB" buffer holds 2168 bytes). This run failed after 158 KB, 27 s
  after `Modem receive buffer full`: data can go missing whenever the buffer sits at the limit,
  not just when the URC comes
- [x] The next run failed in the TLS handshake: `QIOPEN` took 8.6 s, then nothing was read until
  `2168 bytes waiting` 18 s later; the server closed after receiving 2742 bytes of its ~3 KB
  first flight. Data sitting in the module that long means we didn't know it was there: the
  `+QIURC: "recv"` had been lost behind an `OK` (see above), not that we read too slowly
- [x] With the lost URCs handled, the buffer still filled up: `2230 bytes waiting in the modem;
  last read 5683 ms ago, data announced 119 ms ago`, and five more like it over the next 4
  minutes, 3–16 s after the last read each time. The URCs weren't lost, the module sends them
  late (see the polling above). The handshake broke off twice before the third connection got
  through, and resuming took over each time
- [ ] With polling while data flows: whether the buffer still fills up, and the time for the
  whole image
- [ ] ECL 2

Not resumable across a reboot, and about 2 MB per update. Both are fixed by the next step, outside this spec: the server
sends the image in chunks over the device's MQTT session (no HTTP on the device, logs during the
download, the same path on WiFi), then delta patches with `esp_delta_ota`. See
[cornucopia-app#535](https://github.com/cornucopia-machines/cornucopia-app/issues/535) (protocol
and server) and [#674](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/674)
(delta OTA).

### Stage 6 — Use less data and battery, get lower latency

Order to be decided from the stage 3–4 measurements.

- [ ] TLS 1.3 PSK session resumption for MQTT (cheap stopgap from cornucopia-app#492)
- [ ] PSM between wakes for nodes that can tolerate command latency, with a cellular-specific wake/sync cadence (`AT+QSCLK=1` then, for deep sleep)
- [ ] Release the RRC connection sooner after a send: RAI on `QISEND` (`rai=2`, after the reply) or a shorter `AT+QCFG="DataInactTimer"` (default 60 s); see "Power budget"
- [ ] Shorter field names in telemetry payloads (−35% measured, see cornucopia-app#492); needs server changes
- [ ] Server-side handling of the link-quality telemetry fields
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
  UPDATE has to be drained with several `QIRD`s as it arrives. **TCP flow control does not hold
  back the rest:** during an OTA download the module reported `+QIURC: "recv",0,"buff full"`,
  and seven seconds later TLS failed on a record whose MAC didn't verify, so bytes the module's
  TCP stack had already acknowledged were dropped (the app note: "no resources can be allocated
  for incoming data"). What let it fill up was a lost `+QIURC: "recv"`: esp_modem empties its
  buffer when a command completes, so a URC arriving right after an `AT+QIRD`'s `OK`, in the
  same read, was dropped, and the data waited until the module announced the buffer full. That
  is fixed (see stage 5), but the buffer can still overflow if the network delivers faster than
  we read, so anything large over the modem has to survive losing data: a retry, a resumed
  download, or chunks small enough to fit the buffer.
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
  budget (500 MB / 10 years ≈ 137 KB/day). The cellular link uses 10 min for now (a ping every 5
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

The UART runs at 460800, the most the BC660K takes (`AT+IPR`; 115200 is its default). The rate
isn't about NB-IoT's throughput, which 115200 (≈ 11.5 KB/s) easily covers: it's about how fast the
module's 2 KB receive buffer can be emptied. The module drops received data once the buffer is
full instead of holding it back with TCP flow control, and during downloads more than 2 KB
arrived between two reads. A 512-byte `AT+QIRD`, as 1 KB of hex, takes about 90 ms at 115200 and
about 22 ms at 460800 ([#690](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/690)).

- `AT+IPR` takes effect straight away (the `OK` still comes at the old rate) and is saved to the
  module's NVRAM. The module has no autobaud, and nothing documented resets the rate, not even
  `RESET_N`: only another `AT+IPR`.
- So at startup the driver looks for the module at both rates, the fast one first, alternating
  while the module boots. If it answers at 115200, it's switched, and the switch is confirmed
  with an `AT` at the new rate; if that fails, it goes back to 115200.
- The module's I/O runs at 3.3 V (`VIO_SEL` grounded), with no level shifter in between; the
  hardware design guide's 460 kbps limit is for transistor level shifters.
- The UART is clocked from the 40 MHz crystal: a divisor of 86.8125, 0.01% off.
- A light-sleep wake on RX edges takes about 318 µs (see "Waking the ESP32 on UART edges"):
  about 15 bytes at 460800 instead of 4. A `+QIURC: "recv",0,<len>` still leaves enough for an
  unrecognized line, which makes the socket check for data; a short URC like `+CSCON: 0` can
  be lost entirely. To check on the bench.

### Measuring bytes and airtime

- `AT+QENG=2` reports Tx/Rx time (radio-on) since the counters were last reset.
- `AT+QIPERF` gives a baseline throughput figure for the cell we're on.
- On the device, `AtSocketTransport` counts bytes per direction: per connection in the log when
  it closes, and per telemetry interval in the `cellular` telemetry section. These are TLS
  records and everything inside them, but not TCP/IP headers, ACKs or retransmissions.
- 1NCE's portal shows per-SIM data usage. Use it to cross-check the device-side counters.
