# Repository Guidelines

## Project Structure

```text
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

Before building, activate the ESP-IDF environment:

```sh
. tools/activate_idf.sh carrot    # ESP32-C6 (MK10+)
. tools/activate_idf.sh spinach   # ESP32-S3 (MK9)
```

After sourcing, `idf.py` is on `PATH`. Each platform uses a dedicated build
directory to avoid clobbering the other's compiled artifacts and sdkconfig:

```sh
. tools/activate_idf.sh carrot  && idf.py -B build-carrot  build 2>&1 | tail -5
. tools/activate_idf.sh spinach && idf.py -B build-spinach build 2>&1 | tail -5
```

A successful build ends with `Project build complete.` An error prints the compiler diagnostics before the build aborts. **Do not pipe `idf.py build` through `grep` to check success** — grep's exit code replaces idf.py's, making a clean build look like a failure when grep finds no matches.

Pass `-B build-carrot` (or `-B build-spinach`) to every `idf.py` subcommand
(`flash`, `monitor`, `set-target`, `update-dependencies`, etc.) to keep the two
build trees isolated. The platform argument sets `IDF_TARGET`; the build will
error if the sdkconfig in the specified directory was generated for a different
target.

`tools/activate_idf.sh` reads the IDF version from `main/idf_component.yml`. When upgrading IDF, update the version there (and in `components/kernel/idf_component.yml` and `.github/workflows/build.yml`); the script picks it up automatically.

## Coding Style

See [docs/CodingStandards.md](docs/CodingStandards.md) for formatting rules, clang-tidy usage, naming conventions, and Markdown style.

Quick reference:

- LF endings, 4-space indent (2 for JSON/YAML/Markdown). WebKit `.clang-format`, warnings-as-errors via `.clang-tidy`.
- Types: `PascalCase` — functions/methods: `camelCase` — macros/constants: `UPPER_SNAKE`.
- Only remove existing comments if they became obsolete, outdated, or meaningless in the context of the new code. Preserve comments that explain intent, constraints, or non-obvious behavior.

## Wokwi Simulation

See [wokwi/README.md](wokwi/README.md) for diagrams, custom chip authoring (I2C skeleton, callback contract, endianness), build commands, and I2C library interoperability notes.

Quick reference — build a custom chip after editing its `.c` file:

```sh
wokwi-cli chip compile chips/<name>.chip.c -o chips/<name>.chip.wasm
```

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
- **Never modify files under `managed_components/`.** Changes there are not committed, not available on CI, and are silently overwritten by `idf.py update-dependencies`. Fix interoperability issues in our own components instead.
