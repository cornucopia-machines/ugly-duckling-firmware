# Repository Guidelines

## Project Structure & Module Organization

- Firmware entrypoint in `main/` (see `main/main.cpp`); board-specific definitions in `components/devices/src/devices/*.hpp`; shared logic in `components/kernel`, `components/peripherals`, `components/peripherals-api`, `components/utils`, and `components/functions`.
- Test-support utilities live in `components/test-support`; test suites sit under `test/unit-tests`, `test/embedded-tests`, and `test/e2e-tests`.
- NVS config files live in `config/` (with templates in `config-templates/`); Wokwi diagrams in `wokwi/`; docs and examples in `docs/`; generated outputs in `build/`.
- Helper scripts stay at repo root (e.g., `generate-clang-tidy-compile-db.py`, `lookup-backtrace.py`, `idf-docker.py`) and in `scripts/`; CI workflows mirror the local steps.

See [README.md](README.md) for configuration storage layout, build, flash, and development commands.

## Coding Style & Naming Conventions

- Follow `.editorconfig`: LF endings, 4-space indent (2 for JSON/YAML/Markdown). C++ is formatted with WebKit-flavored `.clang-format` (attached braces, left-aligned pointers, no single-line functions).
- Static analysis via `.clang-tidy` (warnings-as-errors). Regenerate the compile DB with `python ./generate-clang-tidy-compile-db.py` and run `clang-tidy -p build/clang-tidy ...`.
- Naming: types in PascalCase (`UglyDucklingMk6`), functions/methods camelCase, constants/macros UPPER_SNAKE (`UD_GEN`, `UD_DEBUG`); keep namespaces compact per config.
- Markdown: keep headings sequential, lists properly indented, and wrap code blocks in fences; follow markdownlint defaults (no trailing spaces, blank line before lists and headings).

## Testing Guidelines

- Add/extend tests alongside the closest suite: fast logic in `test/unit-tests/main`, IDF-integrated flows in `test/embedded-tests`, MQTT/WiFi behavior in `test/e2e-tests`. Prefer deterministic Wokwi fixtures over hardware.
- Use clear test names mirroring the behavior under test; keep payload samples under `config-templates/` or dedicated fixtures instead of inline strings.
- Capture artifacts (`pytest` logs, serial output) when touching connectivity, and document required env vars (`WOKWI_CLI_TOKEN`, `WOKWI_CLI_SERVER` as in CI).

## Commit & Pull Request Guidelines

- Commit messages follow short, imperative summaries (examples: "Add debug property to init message", "Move all test templates to area-51"); keep scope focused.
- PRs should state the target generation (`UD_GEN`/`IDF_TARGET`), what was tested (commands run, tokens used), and any artifacts (logs, Wokwi outputs). Link issues and note firmware or NVS config changes to ease OTA packaging.

## Security & Configuration Tips

- Never commit real MQTT credentials, certificates, or device configs; keep samples under `config-templates/` and load real values into NVS locally or via MQTT NVS commands.
- Prefer editing defaults (`sdkconfig.defaults`, `sdkconfig.*.defaults`) and regenerating `sdkconfig` rather than hand-editing the tracked file; avoid drifting from the CI matrix.
- Keep `dependencies.lock` and `managed_components/` in sync with ESP-IDF tooling; avoid manual edits unless vendoring is intentional.
