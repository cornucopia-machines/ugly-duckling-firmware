# ULP-Based Pulse Counter

## Problem

The original `PulseCounter` implementation uses GPIO interrupts with light sleep. During light sleep,
edge detection is unavailable, so it switches to level-based GPIO wakeup. This means the main CPU
wakes on every single pulse transition — at 1000 Hz that is 2000 wakeups per second, each carrying
the full overhead of exiting and re-entering light sleep.

## Why Not PCNT

The hardware Pulse Counter (PCNT) peripheral requires the APB clock, which is gated during light
sleep. It simply stops counting. It would reduce interrupt overhead while the CPU is actively running,
but does not solve the sleep/wake problem at all.

## Solution: Low-Power Coprocessor

Both supported SoCs include a low-power coprocessor that runs independently of the main CPU, powered
entirely by the RTC/LP domain. It keeps running during light sleep (and deep sleep).

- **ESP32-S3**: ULP-RISC-V coprocessor, RTC domain, ~17.5 MHz
- **ESP32-C6**: LP Core coprocessor, LP domain, ~16 MHz

The design:
- Coprocessor firmware polls configured GPIO pins in a continuous tight loop, detecting falling
  edges on up to `MAX_CHANNELS` channels and incrementing a per-channel counter in RTC/LP slow
  memory. Both ESP32-S3 and ESP32-C6 use the same loop structure with cycle-counting debounce.
- The main CPU wakes only for WiFi DTIM beacons (every 20 beacons ≈ 2 s) and reads the accumulated
  counts from RTC/LP memory at that point. No GPIO-triggered wakeups for pulse counting at all.

## Coprocessor Clock

### ESP32-S3 (ULP-RISC-V)

The ULP-RISC-V runs from the RTC fast clock RC oscillator at ~17.5 MHz
(`ULP_RISCV_CYCLES_PER_US = 17.5` in ESP-IDF). At 17.5 MHz:

- Polling loop throughput: well above Nyquist for 1000 Hz signals.
- 1 ms debounce window = 17 500 cycles at nominal frequency.

The RTC slow clock (≈136 kHz) is not used as the ULP CPU clock; it is only used for RTC timers.

### ESP32-C6 (LP Core)

The LP Core runs from LP_FAST_CLK. The TRM nominally specifies 20 MHz, but ESP-IDF tunes the RC
oscillator to ~16 MHz (`LP_CORE_CPU_FREQUENCY_HZ = 16000000` in ESP-IDF). At 16 MHz:

- Polling loop throughput: well above Nyquist for 1000 Hz signals.
- 1 ms debounce window = 16 000 cycles at nominal frequency.

The debounce conversion happens in the coprocessor firmware during init. Both platforms convert
`debounce_us` to CPU cycles: ESP32-C6 uses `us * 16` (16 cycles/μs at ~16 MHz) and ESP32-S3
uses `us * 17` (17 cycles/μs at ~17.5 MHz).

## Concurrency

The coprocessor only ever **increments** `ulp_pulse_count[i]`; it never resets it. The main CPU only
**reads** `ulp_pulse_count[i]` and tracks `lastSeen` locally. On each `reset()` call:

```
delta = ulp_pulse_count[i] - lastSeen
lastSeen = ulp_pulse_count[i]
return delta
```

This is lock-free by construction. The only possible race is reading a value one increment behind;
the missed pulse appears in the next read. For a flow meter this is entirely acceptable. Aligned
32-bit reads are atomic on the hardware, so torn reads cannot occur.

## Overflow

`ulp_pulse_count[i]` is a `uint32_t`. At 1000 Hz: 2³² / 1000 ≈ 49.7 days to overflow.
The delta subtraction uses unsigned wraparound arithmetic and is self-correcting even through overflow,
as long as the readout interval is much shorter than the overflow period — which it is (seconds vs days).

## Debounce

The main CPU passes a debounce time in microseconds (`ulp_debounce_us`); the coprocessor converts it
to CPU cycles during its init phase using the platform's clock rate. After counting a falling edge,
subsequent edges are ignored until the debounce window has elapsed.

```
debounce_cycles = debounce_us * CYCLES_PER_US
```

Where `CYCLES_PER_US` is 16 (ESP32-C6, ~16 MHz) or 17 (ESP32-S3, ~17.5 MHz).

The RC oscillators are not trimmed to better than ~10%, but for a Hall-effect sensor — which produces
clean, bounce-free transitions — this is entirely sufficient.

## GPIO Constraints

Only RTC/LP-capable GPIOs can be used with the coprocessor path:

- **ESP32-S3**: GPIO 0–21 (`rtc_gpio_is_valid_gpio()` returns true)
- **ESP32-C6**: GPIO 0–7  (`rtc_gpio_is_valid_gpio()` returns true)

`PulseCounterManager::create()` calls `rtc_gpio_is_valid_gpio()` automatically to select the
appropriate implementation. Non-RTC/LP GPIOs fall back to the interrupt-based `GpioPulseCounter`.

The GPIO is configured as an RTC/LP input with pull-down by the main CPU in
`PulseCounterManager::create()` before the coprocessor starts. The coprocessor then re-asserts
input direction during its init phase and polls the level in its main loop.

## Startup Sequence

`PulseCounterManager::create()` registers channels (up to `MAX_CHANNELS = 4`) and configures their
RTC/LP GPIOs, but does not start the coprocessor. `PulseCounterManager::start()` must be called
explicitly after all channels are registered. It zeroes the RTC/LP memory counters (to avoid
carrying over stale values from previous runs), writes the channel configuration, loads the
coprocessor binary, and starts it.

On both platforms the coprocessor runs a continuous polling loop that never returns from `main()`.

### ESP32-S3 startup

```cpp
ulp_riscv_timer_stop();               // stop timer surviving from a previous boot
ulp_riscv_reset();                     // clear any stale trap/done state
ulp_riscv_load_binary(bin_start, size);
ulp_set_wakeup_period(0, 1000);
ulp_riscv_run();
```

The RTC domain survives software resets, so the ULP timer from a previous boot may still be
running. If it fires while `load_binary()` is zeroing RTC memory, the ULP executes illegal
instructions and traps. `ulp_riscv_timer_stop()` + `ulp_riscv_reset()` prevent this.

The ULP timer triggers the first start. Once running, `start.S` calls
`ulp_riscv_rescue_from_monitor()` which clears `DONE` and `SHUT_RESET_EN`, so subsequent
timer ticks are harmlessly ignored. The ULP then enters a continuous polling loop.

### ESP32-C6 startup

```cpp
ulp_lp_core_load_binary(bin_start, size);
ulp_lp_core_cfg_t cfg = { .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU };
ulp_lp_core_run(&cfg);
```

`ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU` means the LP Core is started once by the main CPU and then
runs a continuous tight polling loop until power is removed.

## Public API Changes

The external API of `PulseCounter` and `PulseCounterManager` is unchanged. Existing consumers
(`FlowMeter`, `ElectricFenceMonitor`) require no modification. The only new requirement is that
`PulseCounterManager::start()` is called from the device startup code after all peripherals are
initialized.
