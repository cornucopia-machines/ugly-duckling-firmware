# Cellular on Desert Lark MK2 (nRF9151)

## Summary

Ugly Duckling MK14 connects over LTE-M or NB-IoT through **Desert Lark MK2**: a Nordic nRF9151
running Nordic's [Serial Modem](https://github.com/nrfconnect/ncs-serial-modem) 2.0 firmware, on
the JMODEM1 board-to-board socket. This replaces the Quectel BC660K-GL on Desert Lark MK1 (MK13),
whose driver is gone: MK13 boards run on WiFi only.

The goal is the same as the NB-IoT work's: **telemetry, control and firmware updates over
cellular, to the same MQTT broker, with no server-side changes**, at a power draw a battery-run
device can live with. Most of the layers above the modem driver carry over from that work (see
[`abandoned/NB-IoT.md`](abandoned/NB-IoT.md)); what's new is the modem, its wiring, and how much
simpler both make the driver.

How the link works today is in [`../Connectivity.md`](../Connectivity.md#cellular). This spec
holds the decisions behind it, what was measured, and what's left.

Tracking:

- [#701](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/701) — nRF9151
  driver for MK14, BC660K-GL removal
- [Desert Lark LTE-M roadmap](https://claude.ai/artifact/SmW8BLq2KfWgRFBfsaDiCG) — hardware,
  Desert Lark and firmware roadmap
- Hardware: `docs/architecture.md` § JMODEM1 and `docs/specs/nrf9151-daughter-board.md` in
  ugly-duckling-hardware
- Carried over from the NB-IoT work: [#682](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/682)
  (persistent sessions), [cornucopia-app#533](https://github.com/cornucopia-machines/cornucopia-app/issues/533)
  (choosing the link when provisioning),
  [cornucopia-app#535](https://github.com/cornucopia-machines/cornucopia-app/issues/535) (firmware
  over MQTT in chunks)

## What changed from the BC660K-GL

| | BC660K-GL (MK13) | nRF9151 (MK14) |
| --- | --- | --- |
| RATs | NB-IoT (Cat NB1/NB2) | LTE-M and NB-IoT, configurable order |
| UART | TX/RX only, no flow control, 460800 max | RTS/CTS always on, 921600 |
| Power gating | Module sleeps on its own; ESP32 wakes on RX edges, losing the first bytes | DTR gates the modem's UART; RI wakes the ESP32; nothing is lost |
| Reset | Tied to the ESP32's reset | nRESET on GPIO4, open-drain |
| Socket receive | 2 KB buffer that drops data when full; push mode for downloads | TCP stack holds unread data and throttles the peer |
| Time | `+CTZEU` NITZ, `AT+QNTP` | The modem's clock (`+CCLK`), `%XTIME` NITZ, SNTP over the modem's UDP socket |
| AT layer | esp_modem, plus our own line splitting around it | `AtChannel`, ours |

The BC660K's limits drove most of the complexity in the old driver: hex push mode for updates,
active polling while data flowed, commands held back around pushed URCs, edge-wake byte loss, a
dual-rate probe for a rate saved in NVRAM. Flow control and a modem that keeps what we haven't
read remove all of that.

## Key decisions

### Serial Modem's AT sockets, not PPP

Serial Modem offers PPP (`AT#XPPP`, with CMUX to keep an AT channel alongside), which would make
the modem a regular lwIP netif. We stay with esp-mqtt and `HttpUpdater` over our own
`AtSocketTransport`, as on the BC660K:

- The transport, `TlsTransport` and their wiring into `MqttDriver` and `HttpUpdater` already
  existed and worked; only the socket commands changed.
- PPP keeps the modem's UART up for as long as the link is, which costs 0.4–0.6 mA. With AT
  sockets the UART only runs while there's something to say (see below).
- CMUX plus PPP plus lwIP is more code and RAM in cellular builds, not less.

PPP stays an option if the AT path turns out to limit us. The modem's own MQTT client and TLS
(`AT#XMQTT*`, `AT#XSSOCKET`) were turned down for the same reasons as on the BC660K: a second MQTT
stack, certificates in the modem, and different bytes on the wire than over WiFi.

### No esp_modem

esp_modem was the AT layer for the BC660K. It has no nRF9151 / Serial Modem module, no DTR or RI
handling, and its socket support doesn't fit our transport, so all it would bring is command and
URC framing. We had already worked around that framing on the BC660K: it drops whatever follows
a completed command in the same read, and only empties its buffer when a command returns.
`AtChannel` does the framing directly over the IDF UART driver in about 350 lines: a reader task
splits lines, routes them to the command in flight or to the URC handler, and tracks late results
of commands that timed out. Dropping esp_modem also drops its 67 KB of flash.

### Socket data as hex

`AT#XSEND=<h>,1,0,"<hex>"` and `AT#XRECV=<h>,1,64,0,2048` carry data as hex text, so socket data
goes through the same line handling as every other command. Binary mode would halve the bytes on
the UART, but put raw data in the middle of the line protocol. At 921600 baud, hex still moves
about 46 KB/s of payload, several times what LTE-M delivers.

### DTR and RI for power

Serial Modem turns its UART off while DTR is deasserted (high). The driver asserts DTR, and holds
a no-light-sleep lock, only while commands run and for 100 ms after the last byte either way.
While DTR is off, the modem holds its URCs and asserts RI; RI is a low-level light-sleep wake-up
source on the ESP32, and wakes the driver to assert DTR again. Flow control on both sides means
nothing is lost to a sleeping UART.

- RI is armed only once the modem has answered: it floats while the nRF9151 is in reset or booting.
- RI stuck low for 2 s is taken out of the wake-up sources and polled every 10 s, so a modem stuck
  in reset doesn't keep the ESP32 awake.
- DTR stays off for at least 10 ms between uses, so Serial Modem sees every edge.
- Serial Modem can drop held-back URCs once its UART pipe has been blocked for about 500 ms
  (`sm_at_host_pipe_tx_blocking()`, 5 × 100 ms), so the ESP32 has to answer RI promptly.
- Holding DTR high through ESP32 reset (Desert Lark's pull-up) keeps the ROM boot banner on GPIO16
  out of the modem.

### Reset through nRESET, rarely

nRESET (GPIO4) is open-drain: pulled low or released, never driven high, no pull-up, Hi-Z in sleep
(the nRF9151 has its own 13 kΩ pull-up to 2.2 V, and the pin's ESD diode conducts above about
2.8 V; see #701). It can't be read back.

The driver resets the modem only when it doesn't answer: at startup after the probe fails, or after
3 commands in a row time out. Resets are at least 10 minutes apart, so a modem that never answers
isn't reset in a loop. This is a minimum spacing, not a period, so it doesn't interact with the
task watchdog (15 minutes, fed by the telemetry loop, which doesn't block on the modem).

A modem that restarts on its own (it says `Ready`, or answers at 115200 again) gets its settings
re-applied before the next command, and the open socket is reported closed.

### Baud rate

Serial Modem boots at 115200 with no autobaud, and `AT+IPR` isn't saved. The driver probes at
921600 first (the ESP32 may have restarted without the modem), then 115200, and switches to
921600 with `AT+IPR`. Faster rates aren't worth it: the UART is already faster than the radio.

### RATs and system mode

`cellular.rats` in network-config lists the RATs in order of preference (`["lte-m", "nb-iot"]` by
default). The driver writes `AT%XSYSTEMMODE` (`1,1,0,1` for that default; GNSS off) only when it
differs, in `CFUN=4`, since the modem keeps it in flash and a change costs a fresh network search.
LTE-M comes first by default: lower latency, higher throughput, and every network we've tried so
far serves it.

### DRX by default, eDRX opt-in

`cellular.edrxCycle` defaults to 0, which turns eDRX off (`AT+CEDRXS=0`). The modem then listens
every paging cycle the cell broadcasts (1.28–2.56 s typically), so commands arrive within seconds,
at around 110 µA idle. On the BC660K no network we tried granted eDRX at all; on the bench, One HU
granted 81.92 s on LTE-M to an Onomondo SIM. A nonzero cycle is requested per RAT
(`AT+CEDRXS=2,4,…` for LTE-M, `2,5,…` for NB-IoT; the two have different tables). PSM stays off.

### Release assistance

`AT%RAI=1` at startup lets the transport send `AT#XSOCKETOPT=<h>,1,61,1` (`RAI_NO_DATA`) once an
exchange ends with data received and 5 s of quiet, so the modem leaves RRC connected mode without
waiting for the network's inactivity timer. `rrc-releases` in telemetry counts these. It's only
sent while a single socket is open, since it ends the RRC connection for all of them.

### MQTT stays up during updates

The BC660K driver had one socket, so an update boot downloaded before MQTT was started, with no
logs but the serial console. That was our driver's choice, not a limit of the modem or of
ESP-IDF: over WiFi, MQTT always ran during the download. Serial Modem has a handle per socket and
a TCP stack that holds unread data, so serving two sockets in turn loses nothing. `CellularDriver`
has a transport for MQTT and one for the download, and the update boot works as over WiFi: MQTT
connects first (up to 60 s over the modem, so the two TLS handshakes don't run at once), then the
download starts.

### Time from the modem's clock first

NITZ only comes with an attach, and an ESP32 restart leaves the modem attached, so after an update
or a `restart` no network time came. The driver waited 30 s for it, then up to another 30 s for
the next registration check to ask NTP. On MK14 the time came about 50 s after MQTT connected, and
peripherals are only set up once the time is set. Now:

- At startup, the driver reads the modem's clock (`AT+CCLK?`). The modem sets it from NITZ and
  keeps it while powered; until then it answers `ERROR` (nRF91x1 AT commands v1.4, 5.23).
- An NTP result is written back with `AT+CCLK=`, so the next restart finds the time in the modem
  even on networks without NITZ.
- NTP is asked 10 s after the network is ready, not 30 s, and the network monitor wakes up when
  that's due instead of at its next check.

## Plan

### Driver (#701)

- [x] Extend `CellularModemPins` with RTS, CTS, DTR, RI and reset; MK14 pin map (ESP32 RTS to the
  modem's CTS on GPIO21, CTS from its RTS on GPIO20, DTR GPIO15, RI GPIO3, nRESET GPIO4)
- [x] `AtChannel`: IDF UART driver with flow control, reader task, URC routing, late-result
  tracking, bounded writes (a modem holding CTS can't block a command forever)
- [x] `Nrf9151Driver`: baud probe and switch, settings on every boot, system mode, release
  assistance, PSM/eDRX, SIM and version logging, restart and hang recovery with rate-limited reset
- [x] DTR/RI power management with the no-light-sleep lock
- [x] Sockets: `#XSOCKET` / `#XCONNECT` / `#XSEND` / `#XRECV` / `#XCLOSE`, `#XAPOLL` announcements,
  30 s safety poll, release assistance
- [x] Serving cell from `%XMONITOR` and `%CONEVAL`; `rat` in telemetry and the debug console
- [x] NITZ from `%XTIME`; SNTP over `#XSENDTO` / `#XRECVFROM` as the fallback
- [x] `cellular.rats` in network-config
- [x] Remove `Bc660KDriver`, `EspModem.hpp`, esp_modem, and MK13's cellular pins
- [x] Unit tests for every parser, against lines captured from the modem
- [x] Retry the ICCID and IMSI reads: right after `CFUN=1` the SIM answers `+CME ERROR: 0` for a
  moment
- [x] A socket per transport, and a second transport for firmware downloads: MQTT stays connected
  during an update

### Bench (Desert Lark MK2 on a USB-UART adapter, Onomondo SIM)

The adapter ties DTR low and leaves RI and nRESET unconnected, so this checked the AT dialect, not
the power handling.

- [x] Every command the driver sends, and the shape of every answer it parses (see the captured
  lines in `CellularAtTest.cpp`)
- [x] Socket handles start at 1; `#XRECV` answers `ERROR` with nothing waiting and a bare `OK` at
  end of stream; `#XAPOLL` announces once per drained socket
- [x] `AT+IPR=921600` works and isn't saved
- [x] `+CTZR` is unsupported; `%XTIME` gives NITZ in UTC
- [x] 64 KB HTTP downloads over LTE-M (One HU) at about 115–125 kbit/s
- [ ] 3 of 12 of those downloads ended 5672 bytes short, at the same offset, with a clean close.
  The data never reached the modem's socket; cause unknown (see open questions)

### MK14

- [x] First run (2026-11-04, Onomondo on Yettel HU 21601, LTE-M band 20, roaming; RSRP −109 dBm,
  ECL 1): the modem answers, switches to 921600, takes the new system mode and registers about 4 s
  after it; network ready about 7 s after the modem first answered; TLS handshake 2.9 s, MQTT
  (`wss`) connected 4.8 s after network ready
- [x] Commands and UPDATEs from the server arrive within about 2 s; valve overrides round-trip
- [x] Two hours of telemetry every 5 minutes with no MQTT disconnects and no modem restarts
- [x] DTR gating and RI wake-ups work: announcements and URCs arrive while the modem's UART is off
- [ ] ICCID and IMSI logged at startup with the retry
- [ ] Time from the modem's clock right after an ESP32 restart, and from NTP 10 s after network
  ready on a cold boot without NITZ
- [x] Firmware update over cellular, end to end, with MQTT connected during the download
  (2026-11-04, debug build, LTE-M): 2132 KB in 184 s, about 11.8 KB/s, and the new image booted.
  The modem announced 2694 times; 95 KB (4%) came in through reads it hadn't announced (see "Lost
  announcements"), and no UART errors
- [ ] The free internal RAM during an update over cellular, with both TLS sessions up
- [ ] Modem reset through nRESET: pulse it from a debug build and watch the driver recover
- [x] A release build (`UD_DEBUG` off, `sleepWhenIdle` on) sleeps: `pm.sleep-ratio` 98.18–98.28%
  in three telemetry reports in a row (2026-11-04). The battery current reads 0 without solar
  input: the BQ27220 can't resolve currents that small
- [x] The same release build goes RRC idle, against `mqtt-staging` (not behind Tailscale Funnel,
  whose TCP keepalives page the modem every 15 s; see `abandoned/NB-IoT.md`, "Keepalive and session
  expiry"): `rrc-idle-ratio` 0.90–0.925 and `rrc-longest-connected` 10–12 s over the same three
  reports. The latest report had 3 connections, 3 releases, no missed RRC changes and no MQTT
  disconnects. Debug builds keep the ESP32 awake and publish logs, so they never go RRC idle
- [ ] NB-IoT on MK14 (`"rats": ["nb-iot"]`)
- [ ] Coverage at the sites, with the Desert Lark coverage logger

## Open questions

### Truncated downloads

On the bench, 3 of 12 plain-HTTP 64 KB downloads ended exactly 5672 bytes short, with `#XAPOLL`
POLLHUP and a clean end of stream. Curl over the wired network never truncated. Server, operator
NAT or modem? Updates survive it (TLS catches the gap and the download resumes from what reached
flash), but it costs a TLS handshake each time. To narrow down: the same download over HTTPS, from
another server, and on another operator.

### Lost announcements

The first update over cellular with MQTT connected (MK14, LTE-M) ran at about 2 KB/s. Every 30 s
the safety poll found 2 KB that `#XAPOLL` never announced, so the download stalled for most of each
30 s. That's about one stall per 32 KB, which looks like the modem's TCP window filling up.

Serial Modem 2.0 (`app/src/sm_at_socket.c`) doesn't announce a socket event that comes while
its AT host is busy, meaning running a command or within its idle timer after one. It saves the
event, and once the host is idle it re-registers the socket's one-shot poll callback "to see which
ones are still valid" instead of announcing it. With the download and MQTT (debug logs on every
line) sending commands back to back, the host is rarely idle. Most likely the re-registered
callback doesn't fire for data that's already waiting, and with the window full nothing new
arrives to fire it. The single-socket bench downloads had far less command traffic.

The download transport now reads every 250 ms for as long as data flowed in the last 10 s,
announced or not. The UART is on throughout an update, so the reads cost nothing. With that, the
same download ran at 11–14 KB/s (128 KB every 9–12 s), close to the 115–125 kbit/s of the bench
downloads; the whole 2132 KB image took 184 s, with 4% of it found without an announcement. MQTT keeps relying on announcements and the 30 s safety poll. To confirm the cause: count the bytes "found
without an announcement" in the download's closing log line, and see whether they drop with
`publishLogs` at `Info`.

### Slow sends during cell changes

On the first MK14 run, an `AT#XSEND` of 75 bytes took 12 s, while the modem hopped between three
cells at marginal coverage (TX power at its 20 dBm maximum). Nothing broke, and the broker
redelivered a few QoS 1 messages, which were ignored as duplicates. Worth watching: a send that
blocks that long holds the command mutex, and everything else on the modem waits.

### Band lock

The modem searches every band it supports. A first attach in a new area can take over a minute
(`%MDMEV: SEARCH STATUS 1`, then a full scan). `AT%XBANDLOCK` could narrow it down to the bands
in use (20 and 8 in Hungary so far), but would have to be configurable before devices go
elsewhere.

### Keepalive and the NAT timeout

The keepalive is still 10 minutes (a ping every 5), as set for the 1NCE budget. Onomondo's NAT
idle timeout is unmeasured; the method in `abandoned/NB-IoT.md` ("Keepalive and session expiry")
still applies.

### Modem firmware updates

Serial Modem has `AT#XFOTA` for modem firmware (mfw) and its own application. Not needed yet; the
boards ship with mfw 2.0.4 and Serial Modem 2.0.0.
