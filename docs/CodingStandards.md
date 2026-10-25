# Coding Standards

## Formatting

- Follow `.editorconfig`: LF line endings, UTF-8, 4-space indent (2 for JSON/YAML/Markdown/JS/TS).
- C++ is formatted with a WebKit-flavored `.clang-format`: attached braces, left-aligned pointers (`int* p`), no single-line functions.
- Run `clang-format` before committing; CI will reject misformatted code.

## Static Analysis

- Static analysis is via `.clang-tidy` with warnings-as-errors.
- The clang compile DB (`<build-dir>/clang/compile_commands.json`) is regenerated automatically at the end of every `idf.py build` (`<build-dir>` is whatever `-B` was passed — `build-carrot`/`build-spinach` when building via `tools/build.sh`, which defaults to `build-carrot`).
- Run tidy manually:

  ```bash
  run-clang-tidy -p build-carrot/clang -header-filter="$(pwd)/(main|components)/" <file>
  # or, if you built spinach:
  run-clang-tidy -p build-spinach/clang -header-filter="$(pwd)/(main|components)/" <file>
  ```

- CI runs clang-tidy on every build.

## Binary Size

- Don't call `std::to_string` or `std::to_chars` on a `float` or `double`. Either one links `libstdc++`'s `floating_to_chars.o`, which adds ~123 KB (mostly lookup tables) to the image ([#652](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/652)).
- Use `toStringWithPrecision()` from `Strings.hpp` or `snprintf("%.1f", ...)` instead. `vfprintf` is linked for logging anyway, so these cost nothing extra. For values that are really integers (e.g. Hz to kHz), use integer maths.
- CI fails the build if `floating_to_chars.o` shows up in the link map.

## Error Handling

The firmware is built without C++ exceptions (`CONFIG_COMPILER_CXX_EXCEPTIONS=n`): the unwind tables alone cost more than 100 KB of flash ([#565](https://github.com/cornucopia-machines/ugly-duckling-firmware/issues/565)). Don't use `throw`, `try` or `catch`.

- Anything that can fail because of configuration, attached hardware or the network returns a `Result<T>` (`std::expected<T, Error>`, from `Result.hpp`). The `Error` carries a message for the logs and the init message, so make it say what went wrong in terms of the configuration.
- Propagate errors with `TRY(expr)`: `auto device = TRY(i2c->createDevice(...));`, or `TRY(device->writeRegByte(...));` for a `Result<>`. For `esp_err_t`, use `ESP_TRY(err)` from `EspError.hpp`.
- Classes that can fail to set up get a `static Result<std::shared_ptr<T>> create(...)` factory and a private constructor. Peripheral and function factories may return either the instance or a `Result` of it.
- Configuration values that ArduinoJson converts but which can still be invalid (such as pin names) are rejected by specializing `PropertyValidator<T>`.
- Where a failure can only mean the firmware itself is broken (hardware the board definition hard-codes, resources the firmware allocates at boot), abort with `ESP_ERROR_CHECK` or `esp_system_abort("reason")`, so the crash says why.
- Code that calls into the standard library still aborts on its errors (out-of-range `substr`, constructing a `std::string` from `nullptr`, running out of memory): check the inputs instead.

## Naming Conventions

| Kind | Convention | Example |
| ---- | ---------- | ------- |
| Types (classes, structs, enums) | PascalCase | `UglyDucklingMk6`, `PlotController` |
| Functions and methods | camelCase | `startDevice()`, `getTemperature()` |
| Constants and macros | UPPER_SNAKE | `UD_GEN`, `UD_DEBUG` |
| Namespaces | compact, lowercase | `cornucopia::ugly_duckling::kernel` |

## Markdown

- Keep headings sequential (no skipped levels).
- Lists must be properly indented; blank line before lists and headings.
- Wrap all code examples in fenced code blocks with a language tag.
- No trailing spaces; insert a final newline.
- Follow markdownlint defaults.
