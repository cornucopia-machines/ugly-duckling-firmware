/**
 * ULP/LP-core pulse counter firmware.
 *
 * ESP32-S3 (ULP-RISC-V): halt-based execution driven by the ULP timer.
 *   The coprocessor runs once per timer tick, checks GPIOs, then calls ulp_riscv_halt().
 *   The timer period (set via ulp_set_wakeup_period) defines the polling interval.
 *   State survives across halt/restart cycles because start.S does not zero .bss.
 *
 * ESP32-C6 (LP Core): continuous execution driven by the HP CPU wakeup source.
 *   The LP Core runs an infinite polling loop with cycle-counting debounce.
 *
 * Configuration is written to RTC slow memory by the main CPU before starting the coprocessor.
 * Counts are read from RTC slow memory by the main CPU at any time (lock-free).
 *
 * See docs/ULP-PulseCounter.md for design rationale.
 */

#include <stdint.h>

#ifdef CONFIG_ULP_COPROC_TYPE_LP_CORE
// ESP32-C6 LP Core GPIO API.
// LP_FAST_CLK RC oscillator is tuned to ~16 MHz by ESP-IDF (TRM nominal: 20 MHz).
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"
#define gpio_input_enable(gpio)    ulp_lp_core_gpio_input_enable((lp_io_num_t)(gpio))
#define gpio_get_level(gpio)       ulp_lp_core_gpio_get_level((lp_io_num_t)(gpio))
#else
// ESP32-S3 ULP-RISC-V GPIO API.
// RTC fast clock RC oscillator runs at ~17.5 MHz (ULP_RISCV_CYCLES_PER_US = 17.5).
#include "ulp_riscv_gpio.h"
#include "ulp_riscv_utils.h"
#define gpio_input_enable(gpio)    ulp_riscv_gpio_input_enable(gpio)
#define gpio_get_level(gpio)       ulp_riscv_gpio_get_level(gpio)
#endif

#define MAX_CHANNELS 4

// ---------------------------------------------------------------------------
// Shared with main CPU (written before coprocessor start, read at any time)
// ---------------------------------------------------------------------------

// The ULP build system exports these to the main CPU by prepending "ulp_" to each name,
// producing ulp_channel_count, ulp_gpio_num[], ulp_debounce_us[], ulp_pulse_count[].

/** Number of active channels. Written by main CPU before coprocessor starts. */
volatile uint32_t channel_count;

/** RTC/LP GPIO number for each channel. Written by main CPU before coprocessor starts. */
volatile uint32_t gpio_num[MAX_CHANNELS];

/** Set to 1 on the first instruction of main(). Used by the main CPU to confirm startup. */
volatile uint32_t running;

/**
 * Debounce window in microseconds. Written by main CPU before coprocessor starts.
 *
 * ESP32-S3: converted to restart-period counts on first run (timer period = 1000 µs).
 * ESP32-C6: converted to LP Core CPU cycles on first run.
 */
volatile uint32_t debounce_us[MAX_CHANNELS];

/**
 * Monotonically increasing pulse count per channel.
 * Only ever incremented by the coprocessor. Read by main CPU to compute deltas.
 * Zeroed by main CPU before coprocessor starts to ensure clean state after reboot.
 */
volatile uint32_t pulse_count[MAX_CHANNELS];

// ---------------------------------------------------------------------------

#ifdef CONFIG_ULP_COPROC_TYPE_LP_CORE

// ---------------------------------------------------------------------------
// ESP32-C6 LP Core: continuous execution, cycle-counting debounce
// ---------------------------------------------------------------------------

static inline uint32_t get_ccount(void) {
    uint32_t ccount;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(ccount));
    return ccount;
}

// These only need to survive within the single continuous run, so .bss is fine.
static uint32_t last_level[MAX_CHANNELS];
static uint32_t last_edge_cycle[MAX_CHANNELS];
static uint32_t debounce_cycles[MAX_CHANNELS];

#define DEBOUNCE_CYCLES(us) ((us) * 16u)

int main(void) {
    running = 1;
    uint32_t n = channel_count;

    for (uint32_t i = 0; i < n; i++) {
        gpio_input_enable(gpio_num[i]);
        last_level[i]      = gpio_get_level(gpio_num[i]);
        last_edge_cycle[i] = get_ccount();
        // Convert debounce time from microseconds to coprocessor CPU cycles.
        debounce_cycles[i] = DEBOUNCE_CYCLES(debounce_us[i]);
    }

    while (1) {
        uint32_t now = get_ccount();

        for (uint32_t i = 0; i < n; i++) {
            uint32_t level = gpio_get_level(gpio_num[i]);

            if (level != last_level[i]) {
                last_level[i] = level;

                if (level == 0) {
                    // Falling edge detected. Apply debounce.
                    uint32_t elapsed = now - last_edge_cycle[i];
                    if (elapsed >= debounce_cycles[i]) {
                        pulse_count[i]++;
                        last_edge_cycle[i] = now;
                    }
                }
            }
        }
    }

    return 0;
}

#else

// ---------------------------------------------------------------------------
// ESP32-S3 ULP-RISC-V: halt-based execution, period-counting debounce
//
// start.S does not zero .bss on restart, so all variables survive halt/restart cycles.
//
// last_level: must start as SENTINEL (UINT32_MAX, not 0) after ulp_riscv_load_binary(),
//   so it carries a non-zero initialiser to force placement in .data. On first run we
//   read the actual GPIO level and seed the state; subsequent runs detect edges.
//
// Timer period is set to 1000 µs in PulseCounter.cpp (ulp_set_wakeup_period).
// ---------------------------------------------------------------------------

#define ULP_TIMER_PERIOD_US 1000u

#define SENTINEL UINT32_MAX

static uint32_t last_level[MAX_CHANNELS] = {SENTINEL, SENTINEL, SENTINEL, SENTINEL};
static uint32_t debounce_count[MAX_CHANNELS];

int main(void) {
    running = 1;
    uint32_t n = channel_count;

    for (uint32_t i = 0; i < n; i++) {
        if (last_level[i] == SENTINEL) {
            // First run for this channel: configure GPIO and seed state.
            gpio_input_enable(gpio_num[i]);
            last_level[i]    = gpio_get_level(gpio_num[i]);
            debounce_count[i] = 0;
            continue;
        }

        // Advance debounce countdown.
        if (debounce_count[i] > 0) {
            debounce_count[i]--;
        }

        uint32_t level = gpio_get_level(gpio_num[i]);
        if (level != last_level[i]) {
            last_level[i] = level;
            if (level == 0 && debounce_count[i] == 0) {
                // Falling edge outside debounce window: count and arm debounce.
                pulse_count[i]++;
                debounce_count[i] = debounce_us[i] / ULP_TIMER_PERIOD_US;
            }
        }
    }

    ulp_riscv_halt();
    return 0;
}

#endif
