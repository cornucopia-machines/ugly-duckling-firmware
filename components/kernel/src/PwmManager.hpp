#pragma once

#include <list>

#include <driver/ledc.h>

namespace cornucopia::ugly_duckling::kernel {

// TODO Figure out what to do with low/high speed modes
//      See https://docs.espressif.com/projects/esp-idf/en/release-v4.2/esp32/api-reference/peripherals/ledc.html#ledc-high-and-low-speed-mode

class LedcTimer {
public:
    LedcTimer(ledc_mode_t speedMode, ledc_timer_bit_t dutyResolution, ledc_timer_t timerNum, uint32_t freqHz, ledc_clk_cfg_t clkSrc)
        : speedMode(speedMode)
        , dutyResolution(dutyResolution)
        , timerNum(timerNum)
        , freqHz(freqHz)
        , clkSrc(clkSrc) {
        ledc_timer_config_t config = {
            .speed_mode = speedMode,
            .duty_resolution = dutyResolution,
            .timer_num = timerNum,
            .freq_hz = freqHz,
            .clk_cfg = clkSrc,
            .deconfigure = false,
        };
        ESP_ERROR_THROW(ledc_timer_config(&config));
    }

    ~LedcTimer() {
        ESP_ERROR_CHECK(ledc_timer_rst(speedMode, timerNum));
    }

    bool isSameConfig(ledc_mode_t otherSpeedMode, ledc_timer_bit_t otherDutyResolution, uint32_t otherFreqHz, ledc_clk_cfg_t otherClkCfg) {
        return speedMode == otherSpeedMode && dutyResolution == otherDutyResolution && freqHz == otherFreqHz && clkSrc == otherClkCfg;
    }

    uint32_t constexpr maxValue() const {
        return (1 << dutyResolution) - 1;
    }

private:
    const ledc_mode_t speedMode;
    const ledc_timer_bit_t dutyResolution;
    const ledc_timer_t timerNum;
    const uint32_t freqHz;
    const ledc_clk_cfg_t clkSrc;

    friend class PwmPin;
    friend class PwmManager;
};

class PwmPin {
public:
    PwmPin(const InternalPinPtr& pin, const LedcTimer& timer, ledc_channel_t channel)
        : pin(pin)
        , timer(timer)
        , channel(channel) {

        ledc_channel_config_t config = {
            .gpio_num = pin->getGpio(),
            .speed_mode = timer.speedMode,
            .channel = channel,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = timer.timerNum,
            .duty = 0,    // Set duty to 0%
            .hpoint = 0,
            .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
            .flags = {},
        };
        ESP_ERROR_THROW(ledc_channel_config(&config));
    }

    uint32_t constexpr maxValue() const {
        return timer.maxValue();
    }

    void write(uint32_t value) const {
        ESP_ERROR_THROW(ledc_set_duty(timer.speedMode, channel, value));
        ESP_ERROR_THROW(ledc_update_duty(timer.speedMode, channel));
    }

    const std::string& getName() const {
        return pin->getName();
    }

private:
    const InternalPinPtr pin;
    const LedcTimer& timer;
    const ledc_channel_t channel;

    friend class PmwManager;
};

class PwmManager {
public:
    PwmPin& registerPin(const InternalPinPtr& pin, uint32_t freq, ledc_timer_bit_t dutyResolution = LEDC_TIMER_8_BIT, ledc_clk_cfg_t clkSrc = LEDC_AUTO_CLK) {
        LedcTimer& timer = getOrCreateTimer(LEDC_LOW_SPEED_MODE, dutyResolution, freq, clkSrc);

        auto channel = static_cast<ledc_channel_t>(pins.size());
        if (channel >= LEDC_CHANNEL_MAX) {
            throw std::runtime_error("No more LEDC channels available");
        }

        pins.emplace_back(pin, timer, channel);
        LOGTD("ledc", "Registered PWM channel on pin %s with freq %" PRIu32 " and resolution %d",
            pin->getName().c_str(), freq, dutyResolution);
        return pins.back();
    }

private:
    LedcTimer& getOrCreateTimer(ledc_mode_t speedMode, ledc_timer_bit_t dutyResolution, uint32_t freqHz, ledc_clk_cfg_t clkSrc) {
        for (LedcTimer& timer : timers) {
            if (timer.isSameConfig(speedMode, dutyResolution, freqHz, clkSrc)) {
                return timer;
            }
        }
        auto timerNum = static_cast<ledc_timer_t>(timers.size());
        if (timerNum >= LEDC_TIMER_MAX) {
            throw std::runtime_error("No more LEDC timers available");
        }

        timers.emplace_back(speedMode, dutyResolution, timerNum, freqHz, clkSrc);
        LOGTD("ledc", "Created LEDC timer %d with freq %" PRIu32 " and resolution %d bits",
            timerNum, freqHz, dutyResolution);
        return timers.back();
    }

    std::list<LedcTimer> timers;
    std::list<PwmPin> pins;
};

}    // namespace cornucopia::ugly_duckling::kernel
