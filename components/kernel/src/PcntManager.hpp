#pragma once

#include <limits>

#include <driver/pulse_cnt.h>

#include <EspException.hpp>
#include <utility>

namespace cornucopia::ugly_duckling::kernel {

LOGGING_TAG(PCNT, "pcnt")

/**
 * @brief Counts pulses on a GPIO pin using the Pulse Counter (PCNT) peripheral.
 *
 * Note: This counter is NOT safe to use with the device entering and exiting light sleep.
 * If that behavior is desired, use `PulseCounter` instead.
 */
// TODO Limit number of channels available
struct PulseCounterUnit {
    PulseCounterUnit(pcnt_unit_handle_t unit, InternalPinPtr pin)
        : unit(unit)
        , pin(std::move(pin)) {
    }

    int getCount() const {
        int count;
        pcnt_unit_get_count(unit, &count);
        LOGTV(PCNT, "Counted %d pulses on pin %s",
            count, pin->getName().c_str());
        return count;
    }

    void clear() {
        pcnt_unit_clear_count(unit);
        LOGTV(PCNT, "Cleared counter on pin %s",
            pin->getName().c_str());
    }

    int getAndClearCount() {
        int count = getCount();
        if (count != 0) {
            clear();
        }
        return count;
    }

    PinPtr getPin() const {
        return pin;
    }

private:
    pcnt_unit_handle_t unit;
    const InternalPinPtr pin;
};

class PcntManager {
public:
    static std::shared_ptr<PulseCounterUnit> registerUnit(const InternalPinPtr& pin, nanoseconds maxGlitchDuration = 1000ns) {
        pcnt_unit_config_t unitConfig = {
            .low_limit = std::numeric_limits<int16_t>::min(),
            .high_limit = std::numeric_limits<int16_t>::max(),
            .intr_priority = 0,
            .flags = {},
        };
        pcnt_unit_handle_t unit = nullptr;
        ESP_ERROR_THROW(pcnt_new_unit(&unitConfig, &unit));

        if (maxGlitchDuration != 0ns) {
            pcnt_glitch_filter_config_t filterConfig = {
                .max_glitch_ns = static_cast<uint32_t>(maxGlitchDuration.count()),
            };
            ESP_ERROR_THROW(pcnt_unit_set_glitch_filter(unit, &filterConfig));
        }

        pcnt_chan_config_t channelConfig = {
            .edge_gpio_num = pin->getGpio(),
            .level_gpio_num = -1,
            .flags = {},
        };
        pcnt_channel_handle_t channel = nullptr;
        ESP_ERROR_THROW(pcnt_new_channel(unit, &channelConfig, &channel));
        ESP_ERROR_THROW(pcnt_channel_set_edge_action(channel, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));

        ESP_ERROR_THROW(pcnt_unit_enable(unit));
        ESP_ERROR_THROW(pcnt_unit_clear_count(unit));
        ESP_ERROR_THROW(pcnt_unit_start(unit));

        LOGTD(PCNT, "Registered PCNT unit on pin %s",
            pin->getName().c_str());
        return std::make_shared<PulseCounterUnit>(unit, pin);
    }
};

}    // namespace cornucopia::ugly_duckling::kernel
