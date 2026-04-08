/**
 * ULP/LP-core pulse counter firmware.
 *
 * Both platforms run a continuous polling loop with cycle-counting debounce:
 * - ESP32-S3 (ULP-RISC-V): ~17.5 MHz RTC fast clock.  Started once by the ULP timer;
 *   runs continuously thereafter (never calls ulp_riscv_halt()).
 * - ESP32-C6 (LP Core): ~16 MHz LP_FAST_CLK.  Started once by the HP CPU wakeup source;
 *   runs continuously in an infinite loop.
 *
 * Configuration is written to RTC slow memory by the main CPU before starting the coprocessor.
 * Counts are read from RTC slow memory by the main CPU at any time (lock-free).
 *
 * See docs/ULP-PulseCounter.md for design rationale.
 */

#include <stdint.h>

#ifdef CONFIG_ULP_COPROC_TYPE_LP_CORE
// ESP32-C6 LP Core: ~16 MHz LP_FAST_CLK RC oscillator (TRM nominal: 20 MHz).
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"
#define gpio_input_enable(gpio)    ulp_lp_core_gpio_input_enable((lp_io_num_t)(gpio))
#define gpio_get_level(gpio)       ulp_lp_core_gpio_get_level((lp_io_num_t)(gpio))
#define CYCLES_PER_US 16u
static inline uint32_t get_ccount(void) {
    uint32_t ccount;
    __asm__ __volatile__("csrr %0, mcycle" : "=r"(ccount));
    return ccount;
}
#else
// ESP32-S3 ULP-RISC-V: ~17.5 MHz RTC fast clock RC oscillator.
#include "ulp_riscv_gpio.h"
#include "ulp_riscv_utils.h"
#define gpio_input_enable(gpio)    ulp_riscv_gpio_input_enable(gpio)
#define gpio_get_level(gpio)       ulp_riscv_gpio_get_level(gpio)
#define CYCLES_PER_US 17u
static inline uint32_t get_ccount(void) {
    return ulp_riscv_get_cpu_cycles();
}
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
 * Converted to coprocessor CPU cycles during init on both platforms.
 */
volatile uint32_t debounce_us[MAX_CHANNELS];

/**
 * Monotonically increasing pulse count per channel.
 * Only ever incremented by the coprocessor. Read by main CPU to compute deltas.
 * Zeroed by main CPU before coprocessor starts to ensure clean state after reboot.
 */
volatile uint32_t pulse_count[MAX_CHANNELS];

// ---------------------------------------------------------------------------

static uint32_t last_level[MAX_CHANNELS];
static uint32_t last_edge_cycle[MAX_CHANNELS];
static uint32_t debounce_cycles[MAX_CHANNELS];

int main(void) {
    running = 1;
    uint32_t n = channel_count;

    for (uint32_t gpio_idx = 0; gpio_idx < n; gpio_idx++) {
        gpio_input_enable(gpio_num[gpio_idx]);
        last_level[gpio_idx]      = gpio_get_level(gpio_num[gpio_idx]);
        last_edge_cycle[gpio_idx] = get_ccount();
        debounce_cycles[gpio_idx] = debounce_us[gpio_idx] * CYCLES_PER_US;
    }

    while (1) {
        uint32_t now = get_ccount();

        for (uint32_t gpio_idx = 0; gpio_idx < n; gpio_idx++) {
            uint32_t level = gpio_get_level(gpio_num[gpio_idx]);

            if (level != last_level[gpio_idx]) {
                last_level[gpio_idx] = level;

                if (level == 0) {
                    // Falling edge detected. Apply debounce.
                    uint32_t elapsed = now - last_edge_cycle[gpio_idx];
                    if (elapsed >= debounce_cycles[gpio_idx]) {
                        pulse_count[gpio_idx]++;
                        last_edge_cycle[gpio_idx] = now;
                    }
                }
            }
        }
    }

    return 0;
}
