/**
 * ULP/LP-core pulse counter firmware.
 *
 * Runs continuously at the coprocessor CPU clock regardless of main CPU sleep state.
 * Counts falling edges on up to MAX_CHANNELS RTC/LP-capable GPIO pins.
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
#define DEBOUNCE_CYCLES(us)        ((us) * 16u)
#else
// ESP32-S3 ULP-RISC-V GPIO API.
// RTC fast clock RC oscillator runs at ~17.5 MHz (ULP_RISCV_CYCLES_PER_US = 17.5).
#include "ulp_riscv_gpio.h"
#include "ulp_riscv_utils.h"
#define gpio_input_enable(gpio)    ulp_riscv_gpio_input_enable(gpio)
#define gpio_get_level(gpio)       ulp_riscv_gpio_get_level(gpio)
#define DEBOUNCE_CYCLES(us)        ((us) * 35u / 2u)
#endif

#define MAX_CHANNELS 4

// ---------------------------------------------------------------------------
// Shared with main CPU (written before coprocessor start, read at any time)
// ---------------------------------------------------------------------------

/** Number of active channels. Written by main CPU before coprocessor starts. */
volatile uint32_t ulp_channel_count = 0;

/** RTC/LP GPIO number for each channel. Written by main CPU before coprocessor starts. */
volatile uint32_t ulp_gpio_num[MAX_CHANNELS];

/**
 * Debounce window in microseconds.
 * Converted to coprocessor CPU cycles (using CYCLES_PER_US) during coprocessor init.
 * Written by main CPU before coprocessor starts.
 */
volatile uint32_t ulp_debounce_us[MAX_CHANNELS];

/**
 * Monotonically increasing pulse count per channel.
 * Only ever incremented by the coprocessor. Read by main CPU to compute deltas.
 * Zeroed by main CPU before coprocessor starts to ensure clean state after reboot.
 */
volatile uint32_t ulp_pulse_count[MAX_CHANNELS];

// ---------------------------------------------------------------------------
// Internal coprocessor state (not accessed by main CPU)
// ---------------------------------------------------------------------------

static uint32_t last_level[MAX_CHANNELS];
static uint32_t last_edge_cycle[MAX_CHANNELS];
static uint32_t debounce_cycles[MAX_CHANNELS];

// ---------------------------------------------------------------------------

static inline uint32_t get_ccount(void) {
    uint32_t ccount;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(ccount));
    return ccount;
}

int main(void) {
    uint32_t n = ulp_channel_count;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t gpio = ulp_gpio_num[i];
        // GPIO was initialised as RTC/LP input with pull-down by the main CPU.
        // Re-assert direction here in case of any state change before coprocessor start.
        gpio_input_enable(gpio);
        last_level[i] = gpio_get_level(gpio);
        last_edge_cycle[i] = get_ccount();
        // Convert debounce time from microseconds to coprocessor CPU cycles.
        debounce_cycles[i] = DEBOUNCE_CYCLES(ulp_debounce_us[i]);
    }

    while (1) {
        uint32_t now = get_ccount();

        for (uint32_t i = 0; i < n; i++) {
            uint32_t level = gpio_get_level(ulp_gpio_num[i]);

            if (level != last_level[i]) {
                last_level[i] = level;

                if (level == 0) {
                    // Falling edge detected. Apply debounce.
                    uint32_t elapsed = now - last_edge_cycle[i];
                    if (elapsed >= debounce_cycles[i]) {
                        ulp_pulse_count[i]++;
                        last_edge_cycle[i] = now;
                    }
                }
            }
        }
    }

    // Unreachable, but satisfies the compiler.
    return 0;
}
