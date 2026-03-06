#pragma once

#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <driver/gpio.h>
#include <driver/gpio_filter.h>
#include <esp_adc/adc_oneshot.h>
#include <hal/adc_types.h>

#include <ArduinoJson.h>

#include <EspException.hpp>
#include <Log.hpp>

namespace farmhub::kernel {

class Pin;
using PinPtr = std::shared_ptr<Pin>;

class InternalPin;
using InternalPinPtr = std::shared_ptr<InternalPin>;

/**
 * @brief A GPIO pin abstraction that allows digital reads and writes.
 *
 * @details This can be implemented by internal pins (GPIO pins of the MCU) or external pins provided by external peripherals.
 */
class Pin {
public:
    static PinPtr byName(const std::string& name) {
        auto it = BY_NAME.find(name);
        if (it != BY_NAME.end()) {
            return it->second;
        }
        throw std::runtime_error(std::string("Unknown pin: " + name).c_str());
    }

    enum class Mode : uint8_t {
        Output,
        Input,
        InputPullUp,
        InputPullDown,
    };

    virtual void pinMode(Mode mode) const = 0;

    virtual void digitalWrite(uint8_t val) const = 0;

    virtual int digitalRead() const = 0;

    constexpr const std::string& getName() const {
        return name;
    }

    static void registerPin(const std::string& name, PinPtr pin) {
        BY_NAME[name] = std::move(pin);
    }

    virtual ~Pin() = default;

protected:
    Pin(const std::string& name)
        : name(name) {
    }

    const std::string name;

    static std::map<std::string, PinPtr> BY_NAME;
};

/**
 * @brief An internal GPIO pin of the MCU. These pins can do analog reads as well, and can expose the GPIO number.
 */
class InternalPin : public Pin {
public:
    static InternalPinPtr registerPin(const std::string& name, gpio_num_t gpio) {
        auto pin = std::make_shared<InternalPin>(name, gpio);
        INTERNAL_BY_GPIO[gpio] = pin;
        INTERNAL_BY_NAME[name] = pin;
        Pin::registerPin(name, pin);
        return pin;
    }

    static InternalPinPtr byName(const std::string& name) {
        auto it = INTERNAL_BY_NAME.find(name);
        if (it != INTERNAL_BY_NAME.end()) {
            return it->second;
        }
        throw std::runtime_error(std::string("Unknown internal pin: " + name).c_str());
    }

    static InternalPinPtr byGpio(gpio_num_t pin) {
        auto it = INTERNAL_BY_GPIO.find(pin);
        if (it == INTERNAL_BY_GPIO.end()) {
            std::string name = "GPIO_NUM_" + std::to_string(static_cast<int>(pin));
            return registerPin(name, pin);
        }
        return it->second;
    }

    InternalPin(const std::string& name, gpio_num_t gpio)
        : Pin(name)
        , gpio(gpio) {
    }

    void pinMode(Mode mode) const override {
        gpio_config_t conf = {
            .pin_bit_mask = (1ULL << static_cast<uint8_t>(gpio)),
            .mode = mode == Mode::Output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
            .pull_up_en = mode == Mode::InputPullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = mode == Mode::InputPullDown ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_sleep_set_direction(gpio, conf.mode);
        ESP_ERROR_THROW(gpio_config(&conf));
    }

    void digitalWrite(uint8_t val) const override {
        gpio_set_level(gpio, val);
    }

    int digitalRead() const override {
        return gpio_get_level(gpio);
    }

    IRAM_ATTR void digitalWriteFromISR(uint8_t val) const {
        gpio_set_level(gpio, val);
    }

    IRAM_ATTR int digitalReadFromISR() const {
        return gpio_get_level(gpio);
    }

    constexpr IRAM_ATTR gpio_num_t getGpio() const {
        return gpio;
    }

    /**
     * @brief Enable hardware glitch filter on this pin.
     * Filters out pulses shorter than two IO-MUX clock cycles.
     *
     * @throws EspException if the filter cannot be created or enabled
     */
    void enableGlitchFilter() {
        if (glitchFilter != nullptr) {
            // Already enabled
            return;
        }

        gpio_pin_glitch_filter_config_t filter_config = {
            .clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT,
            .gpio_num = gpio,
        };

        ESP_ERROR_THROW(gpio_new_pin_glitch_filter(&filter_config, &glitchFilter));

        try {
            ESP_ERROR_THROW(gpio_glitch_filter_enable(glitchFilter));
        } catch (...) {
            // Clean up the filter if enable fails
            gpio_del_glitch_filter(glitchFilter);
            glitchFilter = nullptr;
            throw;
        }

        LOGD("Enabled glitch filter for pin %s", name.c_str());
    }

    /**
     * @brief Disable and remove hardware glitch filter from this pin.
     * Multiple calls are safe - will only delete if filter exists.
     *
     * @throws EspException if the filter cannot be disabled or deleted
     */
    void disableGlitchFilter() {
        if (glitchFilter == nullptr) {
            // Already disabled
            return;
        }

        ESP_ERROR_THROW(gpio_glitch_filter_disable(glitchFilter));
        ESP_ERROR_THROW(gpio_del_glitch_filter(glitchFilter));

        glitchFilter = nullptr;
        LOGD("Disabled glitch filter for pin %s", name.c_str());
    }

private:
    const gpio_num_t gpio;
    gpio_glitch_filter_handle_t glitchFilter = nullptr;
    static std::map<std::string, InternalPinPtr> INTERNAL_BY_NAME;
    static std::map<gpio_num_t, InternalPinPtr> INTERNAL_BY_GPIO;
};

class AnalogPin {
public:
    static constexpr double MAX_ANALOG_VALUE = 4096.0;

    AnalogPin(const InternalPinPtr& pin)
        : pin(pin) {
        adc_unit_t unit;
        ESP_ERROR_THROW(adc_oneshot_io_to_channel(pin->getGpio(), &unit, &channel));

        handle = getUnitHandle(unit);

        adc_oneshot_chan_cfg_t config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ESP_ERROR_THROW(adc_oneshot_config_channel(handle, channel, &config));
    }

    ~AnalogPin() {
        ESP_ERROR_CHECK(adc_oneshot_del_unit(handle));
    }

    /**
     * @brief Read an analog value. Throws when reading fails or times out.
     */
    int analogRead() const {
        auto value = tryAnalogRead();
        if (!value) {
            ESP_ERROR_THROW(ESP_ERR_TIMEOUT);
            // Won't get this far
            abort();
        }
        return *value;
    }

    /**
     * @brief Read an analog value as a double. Throws when reading fails or times out.
     */
    double analogReadAsDouble() const {
        return analogRead() / MAX_ANALOG_VALUE;
    }

    /**
     * @brief Read an analog value. Returns `std::nullopt` if the read fails.
     */
    std::optional<int> tryAnalogRead() const {
        int value;
        esp_err_t err = adc_oneshot_read(handle, channel, &value);
        switch (err) {
            case ESP_OK:
                return value;
                break;
            case ESP_ERR_TIMEOUT:
                return std::nullopt;
            default:
                ESP_ERROR_THROW(err);
                // Won't get this far
                abort();
        }
    }

    constexpr const std::string& getName() const {
        return pin->getName();
    }

private:
    static adc_oneshot_unit_handle_t getUnitHandle(adc_unit_t unit) {
        adc_oneshot_unit_handle_t handle = ANALOG_UNITS[unit];
        if (handle == nullptr) {
            adc_oneshot_unit_init_cfg_t config = {};
            config.unit_id = unit;
            ESP_ERROR_THROW(adc_oneshot_new_unit(&config, &handle));
            ANALOG_UNITS[unit] = handle;
        }
        return handle;
    }

    static std::vector<adc_oneshot_unit_handle_t> ANALOG_UNITS;

    const InternalPinPtr pin;
    adc_oneshot_unit_handle_t handle;
    adc_channel_t channel {};
};


}    // namespace farmhub::kernel

namespace ArduinoJson {

using farmhub::kernel::InternalPin;
using farmhub::kernel::InternalPinPtr;
using farmhub::kernel::Pin;
using farmhub::kernel::PinPtr;

template <>
struct Converter<PinPtr> {
    static void toJson(const PinPtr& src, JsonVariant dst) {
        if (src == nullptr) {
            dst.set(nullptr);
        } else {
            dst.set(src->getName());
        }
    }

    static PinPtr fromJson(JsonVariantConst src) {
        if (src.is<const char*>()) {
            return Pin::byName(src.as<const char*>());
        }
        throw std::runtime_error("Invalid pin name: " + src.as<std::string>());
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<const char*>();
    }
};

template <>
struct Converter<InternalPinPtr> {
    static void toJson(const InternalPinPtr& src, JsonVariant dst) {
        if (src == nullptr) {
            dst.set(nullptr);
        } else if (src->getName().starts_with("GPIO_NUM_")) {
            dst.set(static_cast<int>(src->getGpio()));
        } else {
            dst.set(src->getName());
        }
    }

    static InternalPinPtr fromJson(JsonVariantConst src) {
        if (src.is<const char*>()) {
            return InternalPin::byName(src.as<const char*>());
        }
        return InternalPin::byGpio(static_cast<gpio_num_t>(src.as<int>()));
    }

    static bool checkJson(JsonVariantConst src) {
        return src.is<const char*>() || src.is<int>();
    }
};

}    // namespace ArduinoJson
