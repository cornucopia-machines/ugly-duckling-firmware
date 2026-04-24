# Repository Guidelines

## Project Structure

```
main/               # App entry point (main.cpp); MAC-based device selection
components/
  kernel/           # WiFi, MQTT, NTP, RTC, telemetry, NVS, power management
  devices/          # Hardware model definitions (pin assignments, on-board drivers)
  peripherals/      # Sensor and actuator implementations
  peripherals-api/  # Peripheral interfaces (IPeripheral, IValve, …)
  scheduling/       # Scheduling strategies (time, moisture, light, composite)
  functions/        # High-level device logic (PlotController, ChickenDoor)
  utils/            # Shared utilities (Chrono, DebouncedMeasurement)
  test-support/     # Test-only helpers (FakeLog)
test/
  unit-tests/       # Native Catch2 tests (no hardware)
  embedded-tests/   # ESP-IDF Catch2 tests (runs on Wokwi)
  e2e-tests/        # Full MQTT + Wokwi end-to-end tests
config/             # Runtime NVS config files (device-config.json, network-config.json)
config-templates/   # Config templates by device type (never commit real credentials)
wokwi/              # Wokwi diagrams and local dev-env (docker-compose + Mosquitto)
docs/               # Architecture, component, coding standards, and spec documents
scripts/            # Build helpers (gen_config_nvs.py)
tools/              # Development tools
```

See [docs/Architecture.md](docs/Architecture.md) for system-level architecture and the platform/model matrix.
See [docs/Components.md](docs/Components.md) for the peripheral/function/feature model.
See [README.md](README.md) for build, flash, and development commands.

## Build Environment Setup

Before building, activate the ESP-IDF environment and set the target:

```sh
. '/Users/lptr/.espressif/tools/activate_idf_v6.0.sh'
export IDF_TARGET=esp32s3  # or esp32c6, etc.
```

## Coding Style

See [docs/CodingStandards.md](docs/CodingStandards.md) for formatting rules, clang-tidy usage, naming conventions, and Markdown style.

Quick reference:

- LF endings, 4-space indent (2 for JSON/YAML/Markdown). WebKit `.clang-format`, warnings-as-errors via `.clang-tidy`.
- Types: `PascalCase` — functions/methods: `camelCase` — macros/constants: `UPPER_SNAKE`.

## Testing Guidelines

- Fast logic goes in `test/unit-tests/` (native Catch2, no hardware required).
- IDF-integrated flows go in `test/embedded-tests/` (runs on Wokwi).
- MQTT/WiFi behavior goes in `test/e2e-tests/` (full Wokwi + Mosquitto).
- Prefer deterministic Wokwi fixtures over physical hardware.
- Use test names that mirror the behavior under test.
- Keep payload samples in `config-templates/` or dedicated fixtures, not inline strings.
- Document required env vars: `WOKWI_CLI_TOKEN`, `WOKWI_CLI_SERVER`.

## Commit & Pull Request Guidelines

- Commit messages: short imperative summaries (e.g. "Add debug property to init message").
- PRs must state: target platform (`spinach`/`carrot`, `IDF_TARGET`), what was tested (commands run, Wokwi tokens used), and any artifacts (logs, serial output). Link related issues and note NVS config changes.

## Security & Configuration Tips

- Never commit real MQTT credentials, TLS certificates, or device configs. Keep samples in `config-templates/`.
- Prefer editing `sdkconfig.defaults` / `sdkconfig.*.defaults` and regenerating `sdkconfig` rather than hand-editing the tracked file.
- Keep `dependencies.lock` and `managed_components/` in sync with ESP-IDF tooling; avoid manual edits unless intentionally vendoring.
