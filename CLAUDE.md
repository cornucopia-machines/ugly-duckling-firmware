# Repository Guidelines

## Project Structure

```text
main/               # App entry point (main.cpp); device selection (eFuse identity, MAC fallback)
components/
  kernel/           # BLE, WiFi, MQTT, NTP, RTC, telemetry, NVS, power management
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
config/             # network-config.json (NVS-seeded)
config-templates/   # Config templates by device type (never commit real credentials)
wokwi/              # Wokwi diagrams and local dev-env (docker-compose + Mosquitto)
docs/               # Architecture, component, coding standards, and spec documents
scripts/            # Build helpers (gen_config_nvs.py)
tools/              # Development tools (build/activate wrappers, eFuse burn/verify CLI, kalman notebook, …)
  test/             # Python tests for tools/ scripts (stdlib unittest, no ESP-IDF build required)
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
. tools/activate_idf.sh carrot  && idf.py -B build-carrot  build > /tmp/build-carrot.log  2>&1; echo "exit: $?"
. tools/activate_idf.sh spinach && idf.py -B build-spinach build > /tmp/build-spinach.log 2>&1; echo "exit: $?"
```

A successful build ends with `Project build complete.` and exits 0. Redirect to a log file rather than piping through `tail`: the CMake configure step and linker script listing are thousands of tokens of noise that add no value once the build succeeds, and (unlike `tail -N`) a redirect never truncates the diagnostics you actually need on failure. Check the exit code first; only `grep`/`tail` the log file when the build fails. **Do not pipe `idf.py build` through `grep` directly to check success** — grep's exit code replaces idf.py's, making a clean build look like a failure when grep finds no matches.

Pass `-B build-carrot` (or `-B build-spinach`) to every `idf.py` subcommand
(`flash`, `monitor`, `set-target`, `update-dependencies`, etc.) to keep the two
build trees isolated. The platform argument sets `IDF_TARGET`; the build will
error if the sdkconfig in the specified directory was generated for a different
target. `sdkconfig` is generated inside the build directory (e.g.
`build-carrot/sdkconfig`), not in the project root.

**An existing `build-*/sdkconfig` takes precedence over `sdkconfig*.defaults`.** The defaults files
only seed a config that does not exist yet, so editing them does *not* change a symbol that is
already present in a generated `sdkconfig` — not even after `idf.py reconfigure`. To pick up a
changed default locally, delete the generated file and let it be regenerated:

```sh
rm build-carrot/sdkconfig && . tools/activate_idf.sh carrot && idf.py -B build-carrot reconfigure
```

Always verify the symbol actually changed in `build-<platform>/sdkconfig` afterwards. CI builds
from scratch, so it only ever sees the defaults — a local build that silently kept the old value
will disagree with CI.

`tools/activate_idf.sh` reads the IDF version from `main/idf_component.yml`. When upgrading IDF, update the version there (and in `components/kernel/idf_component.yml` and `.github/workflows/build.yml`); the script picks it up automatically.

`tools/build.sh [carrot|spinach] [idf.py args...]` wraps the two steps above:
it sources `activate_idf.sh` for the given platform and runs `idf.py` with
`-B build-<platform>` already set, forwarding any remaining arguments. The
platform argument is optional and defaults to `carrot` when omitted. Like
`activate_idf.sh`, it must be **sourced**, not executed, and run from the
project root — IDF's own activation script only exports its environment
variables when it detects it's sourced directly by an interactive shell, so
nested inside an executed script it refuses to run:

```sh
. tools/build.sh build                          # carrot (default)
. tools/build.sh spinach -DUD_NETWORK_CONFIG=config/network-config.prod.json build app-flash monitor
```

## Coding Style

See [docs/CodingStandards.md](docs/CodingStandards.md) for formatting rules, clang-tidy usage, naming conventions, and Markdown style.

Quick reference:

- LF endings, 4-space indent (2 for JSON/YAML/Markdown). WebKit `.clang-format`, warnings-as-errors via `.clang-tidy`.
- Types: `PascalCase` — functions/methods: `camelCase` — macros/constants: `UPPER_SNAKE`.
- Only remove existing comments if they became obsolete, outdated, or meaningless in the context of the new code. Preserve comments that explain intent, constraints, or non-obvious behavior.

Quick reference — run clang-tidy locally (requires a prior build for the compile commands DB):

```sh
. tools/activate_idf.sh carrot   # or spinach
run-clang-tidy -p build-carrot/clang -header-filter="$(pwd)/(main|components)/" \
  $(find main components -type f -name '*.cpp' -not -path '*/test/*' -not -path '*/build-carrot/*')
```

Add `-fix` to auto-apply fixes; since `.clang-tidy` sets `WarningsAsErrors: "*"`,
fixes won't apply unless you temporarily change it to `WarningsAsErrors: ""` (restore
after the run). Tidy must be run per platform — each target sees only its own
`#ifdef` branches, so both carrot and spinach runs are needed for full coverage.

## Wokwi Simulation

See [wokwi/README.md](wokwi/README.md) for diagrams, custom chip authoring (I2C skeleton, callback contract, endianness), build commands, and I2C library interoperability notes.

Quick reference — build a custom chip after editing its `.c` file:

```sh
wokwi-cli chip compile chips/<name>.chip.c -o chips/<name>.chip.wasm
```

## Resolving Crash Backtraces

Use `lookup-backtrace.py` to symbolize a `Guru Meditation Error` backtrace. Paste the `PC:`
value and the full `Backtrace:` line on stdin; it prints source locations, including inlined
frames by default (inlined frames are frequently exactly where the crash is — lambdas,
`shared_ptr` accessors, and small getters are routinely inlined).

```sh
python3 lookup-backtrace.py path/to/the-exact-crashing.elf <<< "$(pbpaste)"
# or interactively: python3 lookup-backtrace.py path/to/the-exact-crashing.elf, then paste and Ctrl-D
```

The ELF argument is optional (defaults to `build/ugly-duckling.elf`), but must be the exact
build that produced the log (matching version string) — not a fresh local `build-*/` output.
The script picks the right `addr2line` binary in this order: `--target esp32|esp32s3|esp32c6`
if passed, else the `IDF_TARGET` env var (set by `tools/activate_idf.sh carrot|spinach`), else
a guess from the ELF path/filename (`carrot`/`esp32c6` → RISC-V, `spinach`/`esp32s3` → Xtensa
S3, else classic Xtensa ESP32). Pass `--addr2line` to bypass all of that and name the binary
directly. Requires the matching IDF toolchain on `PATH` either way.

## Hardware Identity (eFuse)

See [docs/specs/done/Hardware-Version-in-eFuse.md](docs/specs/done/Hardware-Version-in-eFuse.md)
for the full design: record layout, endianness, why there's no app-level CRC,
and how device selection prefers the eFuse identity over MAC prefix matching
when present (Carrot / ESP32-C6 only; applies to MK11 onward).

Quick reference — burn/verify at board test (`--chip` auto-detects; `--port`
is always required, `espefuse` has no auto-detection for it):

```sh
tools/efuse_burn.py identity --port /dev/ttyUSB0 --hw-gen 11 --hw-rev 1 --mfr-id 0x01 --batch 0z70kbl --serial 0x1042
# or, from JLCPCB's assembly label QR code (defaults --mfr-id to JLCPCB):
tools/efuse_burn.py identity --port /dev/ttyUSB0 --jlcpcb-qr UD11R01_70kbl_1042
tools/efuse_burn.py show --port /dev/ttyUSB0
```

## Testing Guidelines

- Fast logic goes in `test/unit-tests/` (native Catch2, no hardware required).
- IDF-integrated flows go in `test/embedded-tests/` (runs on Wokwi).
- MQTT/WiFi behavior goes in `test/e2e-tests/` (full Wokwi + Mosquitto).
- Python CLI tooling tests go in `tools/test/` (stdlib `unittest`; for
  `efuse_burn.py` this runs against `espefuse --virt`, no real hardware or
  ESP-IDF build required — just `espefuse` on `IDF_PYTHON_ENV_PATH`).
- Prefer deterministic Wokwi fixtures over physical hardware.
- Use test names that mirror the behavior under test.
- Keep payload samples in `config-templates/` or dedicated fixtures, not inline strings.
- Document required env vars: `WOKWI_CLI_TOKEN`, `WOKWI_CLI_SERVER`. If `test/.wokwi-env` exists locally
  (gitignored — it holds a real token, never commit it), `source` it to populate both before running
  `embedded-tests`/`e2e-tests`.
- For local `idf.py build` verification, testing `carrot` alone is enough for most changes. If a change plausibly affects `spinach` specifically (e.g. Spinach-only devices/peripherals, BLE, or other platform-conditional code), ask before also building `spinach` rather than assuming it's needed.

## Commit & Pull Request Guidelines

- Commit messages: short imperative summaries (e.g. "Add debug property to init message").
- PRs must state: target platform (`spinach`/`carrot`, `IDF_TARGET`), what was tested (commands run, Wokwi tokens used), and any artifacts (logs, serial output). Link related issues and note NVS config changes.

## Security & Configuration Tips

- Never commit real MQTT credentials, TLS certificates, or device configs. Keep samples in `config-templates/`.
- Prefer editing `sdkconfig.defaults` / `sdkconfig.*.defaults` and regenerating `sdkconfig` rather than hand-editing the tracked file.
- Keep `dependencies.lock` and `managed_components/` in sync with ESP-IDF tooling; avoid manual edits unless intentionally vendoring.
- **Never modify files under `managed_components/`.** Changes there are not committed, not available on CI, and are silently overwritten by `idf.py update-dependencies`. Fix interoperability issues in our own components instead.
