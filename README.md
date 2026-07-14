# Ugly Duckling

![Build badge](https://github.com/cornucopia-machines/ugly-duckling/actions/workflows/build.yml/badge.svg)

Ugly Duckling is a firmware for IoT devices participating in the Cornucopia ecosystem like the GrowMachine.

The devices are built around the Espressif ESP32 micro-controller using the ESP-IDF framework and FreeRTOS.
The devices can report telemetry to, and receive configuration and commands from the Cornucopia server via MQTT over WiFi.
They can also receive firmware updates via HTTP(S).

Devices are identified by a location (denoting the installation site) and a unique instance name.
Devices can have multiple peripherals, such as sensors, actuators, and displays.
Each peripheral is identified by its type and a name that is unique within the device.

See [docs/Architecture.md](docs/Architecture.md) for the system architecture and [docs/Components.md](docs/Components.md) for the peripheral/function model.

## Hardware platforms and models

Ugly Duckling hardware is organised into two layers:

### Platforms

A **platform** is an abstract hardware environment that a firmware binary is built for.
Platforms are named independently of the underlying chip, so multiple distinct platforms can target the same chip — a new board family on the same silicon gets its own platform name rather than sharing a binary with an unrelated board.
Each platform maps to one ESP-IDF target (and therefore one compiled binary), but the name expresses the role of the hardware rather than its silicon.

There are currently two platforms:

* `spinach` is based on ESP32-S3
  * MK5 (rev2)
  * MK6 (rev1–rev3)
  * MK7 (rev1)
  * MK8 (rev1)
  * MK9 (rev1)
* `carrot` is based on ESP32-C6
  * MK10 (rev1)
  * MK11 (rev1)

Each platform produces a single firmware binary that covers all models on that platform.
The correct model is selected at runtime, preferring a hardware identity record burned
into eFuse at board test (Carrot / MK11 onward) and falling back to the device's MAC
address prefix when that record isn't present — see
[docs/specs/Hardware-Version-in-eFuse.md](docs/specs/Hardware-Version-in-eFuse.md) for
the eFuse record layout, the burn/verify CLI, and why MAC-only detection stopped being
reliable once MK11 shipped with MAC ranges overlapping MK10's.

### Models

A **model** identifies a specific PCB hardware revision.
Within a model, **revisions** track incremental hardware changes (e.g. different motor nSLEEP pin routing on MK6 rev1/rev2 vs rev3).
Both the model and revision are reported in boot logs and in the MQTT `init` message (`model`, `revision`, and `platform` fields), so operators can identify exactly what is running without inspecting the MAC address.

## Network configuration

Network configuration is stored under the `network-config` key in NVS (the `config` namespace).
It includes both the device identity and the MQTT broker settings:

```jsonc
{
  "instance": "chicken-door", // the instance name, defaults to MAC address
  "location": "my-garden", // the name of the location the device is installed at
  "host": "...", // broker host name
  "port": 1883, // broker port, defaults to 1883
  "clientId": "chicken-door", // client ID, defaults to "ugly-duckling-$instance" if omitted
  "queueSize": 16, // MQTT message queue size, defaults to 16
  "ntp": {
    "host": "pool.ntp.org", // NTP server host name, optional
  },
}
```

Ugly Duckling supports TLS-encrypted MQTT connections using client-side certificates.
To enable this, the following parameters must be present in the network config:

```jsonc
{
  // ...
  "serverCert": [
    "-----BEGIN CERTIFICATE-----",
    "...",
    "-----END CERTIFICATE-----",
  ],
  "clientCert": [
    "-----BEGIN CERTIFICATE-----",
    "...",
    "-----END CERTIFICATE-----",
  ],
  "clientKey": [
    "-----BEGIN RSA PRIVATE KEY-----",
    "...",
    "-----END RSA PRIVATE KEY-----",
  ],
}
```

The certificates and keys must be in Base64 encoded PEM format, each line must be a separate element in an array.

## Device configuration

Hardware configuration is stored under the `device-config` key in NVS (the `config` namespace).
This describes the device peripherals and functions.

```jsonc
{
  "peripherals": [
    {
      "type": "chicken-door",
      "name": "main-coop-door",
      "params": {
        "motor": "b",
        "openPin": "B2",
        "closedPin": "B1",
        "lightSensor": {
          "scl": "C2",
          "sda": "C3",
        },
      },
    },
  ],
}
```

Devices communicate using the topic `/devices/ugly-duckling/$DEVICE_INSTANCE`, or `$DEVICE_ROOT` for short.
For example, during boot, the device will publish a message to `/devices/ugly-duckling/$DEVICE_INSTANCE/init`, or `$DEVICE_ROOT/init` for short.

Peripherals communicate using the topic `$DEVICE_ROOT/peripheral/$PERIPHERAL_NAME`, or `$PERIPHERAL_ROOT` for short.

## Peripheral configuration

Some peripherals can receive custom configurations, for example, a flow controller can have a custom schedule.
These are communicated via MQTT under the `$PERIPHERAL_NAME/config` topic.
Once the device receives such configuration, it stores it under the `$FUNCTION_NAME` key in the `function-cfg` NVS namespace.

## Remote commands

Ugly duckling devices and their peripherals both support receiving commands via MQTT.
Commands are triggered via retained MQTT messages sent to `$DEVICE_ROOT/commands/$COMMAND` for devices, and `$PERIPHERAL_ROOT/commands/$COMMAND` for peripherals.
They typically respond under `$DEVICE_ROOT/responses/$COMMAND`.

Once the device receives a command it deletes the retained message.
This allows commands to be sent to sleeping devices.

There are a few commands supported out-of-the-box:

### Echo

Whatever JSON you send to `$DEVICE_ROOT/commands/echo`.

See `EchoCommand` for more information.

### Restart

Sending a message to `$DEVICE_ROOT/commands/restart` restarts the device immediately.

See `RestartCommand` for more information.

### Firmware update via HTTP

Sending a message to `$DEVICE_ROOT/commands/update` with a URL to a firmware binary (`firmware.bin`), it will instruct the device to update its firmware:

```jsonc
{
  "url": "https://github.com/.../.../releases/download/.../firmware.bin",
}
```

See `HttpUpdateCommand` for more information.

### NVS commands

The following commands are available to manage NVS entries on the device:

- `commands/nvs/list` returns a list of keys in the `config` namespace
- `commands/nvs/read` reads the value at the given `key`
- `commands/nvs/write` writes the given `value` to the given `key`
- `commands/nvs/remove` removes the entry at the given `key`

## Development

### Prerequisites

- [ESP-IDF-Clang Docker image](https://github.com/lptr/esp-idf-clang-docker) (wraps ESP-IDF v6.0.2 with Clang toolchain)

### Building

Set `IDF_TARGET` to the chip for your platform (`esp32s3` for Spinach, `esp32c6` for Carrot) and build:

```bash
idf.py build
```

The resulting binary uses runtime MAC detection to select the correct model at boot.
No `UD_GEN` is required for normal builds.

You can add `-DUD_DEBUG=1` to enable debug output:

```bash
idf.py build -DUD_DEBUG=1
```

You can add `-DUD_PM_DIAGNOSTICS=1` to enable power-management diagnostics — a periodic
(every 2.5s) dump of PM lock hold times, `esp_timer` stats, light-sleep wakeup count/causes,
and per-task CPU time to the console. Real overhead (~9% CPU in testing), not for production
builds. Separate from `UD_DEBUG` since debug builds disable light sleep entirely, which would
make PM diagnostics pointless. See `components/kernel/src/PowerManager.hpp` and
`sdkconfig.pm_diagnostics.defaults`:

```bash
idf.py build -DUD_PM_DIAGNOSTICS=1
```

#### Pinning to a specific model (development / simulation)

Pass `UD_GEN` to skip MAC detection and force a specific model.
Make sure `IDF_TARGET` matches the platform the model belongs to:

```bash
idf.py build -DUD_GEN=MK6_REV3       # Spinach (ESP32-S3)
idf.py build -DUD_GEN=MK10_REV1      # Carrot (ESP32-C6)
```

### Flashing

```bash
idf.py flash
```

This flashes the full firmware: the bootloader, the partition table, the app, and the NVS config partition (generated from `config/device-config.json` and `config/network-config.json`).

> [!WARNING]
> Flashing the NVS partition will erase all NVS data on the device, including WiFi credentials.
> This is intended for initial device setup or reconfiguration.

To flash only the app (leaving the existing NVS partition untouched), use:

```bash
idf.py app-flash
```

#### Uploading just config

To upload only the NVS config partition:

```bash
idf.py build
esptool.py write_flash 0x18000 build/config.bin
```

Alternatively, use the NVS commands over MQTT to update individual keys without erasing the device:

```bash
# Write device config
mosquitto_pub -t "$DEVICE_ROOT/commands/nvs/write" -m '{"key":"device-config","value":{...}}'
# Write network config
mosquitto_pub -t "$DEVICE_ROOT/commands/nvs/write" -m '{"key":"network-config","value":{...}}'
```

### Hardware identity (eFuse)

Carrot boards (MK11 onward) get a one-time hardware identity record burned into
eFuse at board test — generation, revision, manufacturer ID, and serial number.
This is what device selection prefers over MAC address matching (see
[Platforms](#platforms) above). Burn and verify it with `tools/efuse_burn.py`,
which auto-detects `--chip` from the connected board if not given (`--port` is
always required — `espefuse` doesn't auto-detect it):

```bash
tools/efuse_burn.py identity --port /dev/ttyUSB0 --hw-gen 11 --hw-rev 0 --mfr-id 0x01 --batch 0z70kbl --serial 0x1042
tools/efuse_burn.py show --port /dev/ttyUSB0
```

See [docs/specs/Hardware-Version-in-eFuse.md](docs/specs/Hardware-Version-in-eFuse.md)
for the record layout and design rationale.

### Monitoring

```bash
idf.py monitor
```

### Simulation

Can use [Wokwi](https://wokwi.com/) to run the firmware in a simulated environment.
For this the firmware must be built with `-DWOKWI=1`.

```bash
idf.py -DUD_GEN=MK6_REV3 -DUD_DEBUG=0 -DWOKWI=1 build
```

The opening a diagram in the [`wokwi`](wokwi) directory will start the simulation.

#### Debugging with Wokwi

To start the simulation with the debugger enabled, place a breakpoint, then hit `Cmd+Shift+P` and select `Wokwi: Start Simulator and Wait for Debugger`.
After that from the "Run and Debug" panel select the "Wokwi GDB" configuration and hit the play button.

### Testing

There are four test suites:

#### Unit tests (native, no hardware required)

```bash
cd test/unit-tests
cmake -B build -G Ninja
ninja -C build
./build/unit-tests
```

#### Embedded tests (Wokwi simulator)

```bash
cd test/embedded-tests
idf.py build
pytest --embedded-services idf,wokwi pytest_embedded-tests.py
```

#### End-to-end tests (Wokwi + MQTT)

```bash
cd test/e2e-tests
idf.py build
pytest pytest_e2e-tests.py
```

Set `WOKWI_CLI_TOKEN` (and optionally `WOKWI_CLI_SERVER`) before running Wokwi-based tests.

If `pytest` is not available, install it using the ESP-IDF Python environment:

```bash
./install.sh --enable-pytest
pip install pytest-embedded-wokwi
```

#### Tools tests (native, no hardware required)

Tests for scripts under `tools/`, e.g. `tools/efuse_burn.py` (run against `espefuse --virt`):

```bash
python3 -m unittest discover -s tools/test -v
```
