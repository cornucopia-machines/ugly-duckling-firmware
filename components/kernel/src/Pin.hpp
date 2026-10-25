#pragma once
#include <EspError.hpp>
#include <Log.hpp>
#include <config/Configuration.hpp>

#include <ArduinoJson.h>
#include <driver/gpio.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <hal/adc_types.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cornucopia::ugly_duckling::kernel {

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
    /**
     * @brief Look up a pin by name; nullptr if the board has no such pin.
     */
    static PinPtr byName(const std::string& name) {
        auto it = BY_NAME.find(name);
        if (it != BY_NAME.end()) {
            return it->second;
        }
        return nullptr;
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

    inline static std::map<std::string, PinPtr> BY_NAME;
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

    /**
     * @brief Look up an internal pin by name; nullptr if the board has no such pin.
     */
    static InternalPinPtr byName(const std::string& name) {
        auto it = INTERNAL_BY_NAME.find(name);
        if (it != INTERNAL_BY_NAME.end()) {
            return it->second;
        }
        return nullptr;
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
        // Only fails for an invalid GPIO number, which no InternalPin has
        ESP_ERROR_CHECK(gpio_config(&conf));
    }

    /**
     * @brief Drive this pin at a known level without claiming ownership of it.
     *
     * Use this to park a pin during boot when a driver (LEDC, RMT, ...) is meant
     * to take the pin over later on. pinMode() goes through gpio_config(), which
     * reserves every pin it configures as an output; the driver claiming the pin
     * afterwards then finds it reserved and warns about a conflict. Configuring
     * the pad directly avoids taking that reservation in the first place, so the
     * later hand-over stays clean.
     *
     * Pulls are disabled explicitly, since unlike gpio_config() the calls below
     * leave whatever pull configuration the pad happens to carry.
     */
    void parkOutput(uint8_t level) const {
        ESP_ERROR_CHECK(gpio_set_pull_mode(gpio, GPIO_FLOATING));
        // Set the level first so enabling the output driver doesn't glitch the
        // pin through whatever the output register happened to hold.
        ESP_ERROR_CHECK(gpio_set_level(gpio, level));
        gpio_sleep_set_direction(gpio, GPIO_MODE_OUTPUT);
        ESP_ERROR_CHECK(gpio_set_direction(gpio, GPIO_MODE_OUTPUT));
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

private:
    const gpio_num_t gpio;
    inline static std::map<std::string, InternalPinPtr> INTERNAL_BY_NAME;
    inline static std::map<gpio_num_t, InternalPinPtr> INTERNAL_BY_GPIO;
};

class AnalogPin {
public:
    static constexpr double MAX_ANALOG_VALUE = 4096.0;

    /**
     * @brief Set up `pin` for analog reads; fails if the pin cannot do ADC.
     */
    static Result<std::unique_ptr<AnalogPin>> create(
        const InternalPinPtr& pin,
        adc_atten_t atten = ADC_ATTEN_DB_12,
        adc_bitwidth_t bitwidth = ADC_BITWIDTH_DEFAULT) {
        adc_unit_t unit;
        adc_channel_t channel {};
        esp_err_t err = adc_oneshot_io_to_channel(pin->getGpio(), &unit, &channel);
        if (err != ESP_OK) {
            return failure("Pin " + pin->getName() + " cannot be used for analog reads: " + esp_err_to_name(err));
        }

        adc_oneshot_unit_handle_t handle = TRY(getUnitHandle(unit));

        LOGI("Initializing analog pin %s (GPIO %d) with attenuation %d and bitwidth %d",
            pin->getName().c_str(), static_cast<int>(pin->getGpio()), static_cast<int>(atten), static_cast<int>(bitwidth));

        adc_oneshot_chan_cfg_t config = {
            .atten = atten,
            .bitwidth = bitwidth,
        };
        ESP_TRY(adc_oneshot_config_channel(handle, channel, &config));

        adc_cali_curve_fitting_config_t caliConfig = {
            .unit_id = unit,
            .chan = channel,
            .atten = atten,
            .bitwidth = bitwidth,
        };
        adc_cali_handle_t caliHandle = nullptr;
        esp_err_t ret = adc_cali_create_scheme_curve_fitting(&caliConfig, &caliHandle);
        if (ret != ESP_OK) {
            LOGW("ADC calibration unavailable for pin %s (%s)",
                pin->getName().c_str(), esp_err_to_name(ret));
            caliHandle = nullptr;
        }
        return std::unique_ptr<AnalogPin>(new AnalogPin(pin, channel, handle, caliHandle));
    }

    AnalogPin(const AnalogPin&) = delete;
    AnalogPin& operator=(const AnalogPin&) = delete;

    ~AnalogPin() {
        if (caliHandle != nullptr) {
            ESP_ERROR_CHECK(adc_cali_delete_scheme_curve_fitting(caliHandle));
        }
        // The unit handle is shared with every other pin on the unit, and stays in ANALOG_UNITS
    }

    /**
     * @brief Read an analog value as a double. Returns `std::nullopt` if the read fails.
     */
    std::optional<double> tryAnalogReadAsDouble() const {
        auto value = tryAnalogRead();
        if (!value) {
            return std::nullopt;
        }
        return *value / MAX_ANALOG_VALUE;
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
            case ESP_ERR_TIMEOUT:
                return std::nullopt;
            default:
                LOGW("Analog read on pin %s failed: %s",
                    pin->getName().c_str(), esp_err_to_name(err));
                return std::nullopt;
        }
    }

    /**
     * @brief Read calibrated voltage in millivolts. Returns `std::nullopt` if the read fails
     * , or when eFuse curve-fitting calibration is not available.
     */
    std::optional<int> tryAnalogReadMillivolts() const {
        if (caliHandle == nullptr) {
            return std::nullopt;
        }
        auto raw = tryAnalogRead();
        if (!raw.has_value()) {
            return std::nullopt;
        }
        int mv;
        esp_err_t err = adc_cali_raw_to_voltage(caliHandle, *raw, &mv);
        if (err != ESP_OK) {
            LOGW("Converting analog read on pin %s to voltage failed: %s",
                pin->getName().c_str(), esp_err_to_name(err));
            return std::nullopt;
        }
        LOGV("Analog read on pin %s (GPIO %d): raw=%d, voltage=%d mV",
            pin->getName().c_str(), static_cast<int>(pin->getGpio()), *raw, mv);
        return mv;
    }

    constexpr const std::string& getName() const {
        return pin->getName();
    }

private:
    AnalogPin(const InternalPinPtr& pin, adc_channel_t channel, adc_oneshot_unit_handle_t handle, adc_cali_handle_t caliHandle)
        : pin(pin)
        , channel(channel)
        , handle(handle)
        , caliHandle(caliHandle) {
    }

    static Result<adc_oneshot_unit_handle_t> getUnitHandle(adc_unit_t unit) {
        adc_oneshot_unit_handle_t handle = ANALOG_UNITS[unit];
        if (handle == nullptr) {
            adc_oneshot_unit_init_cfg_t config = {};
            config.unit_id = unit;
            ESP_TRY(adc_oneshot_new_unit(&config, &handle));
            ANALOG_UNITS[unit] = handle;
        }
        return handle;
    }

    // Created on first use, and kept for good: there are at most two units
    inline static std::vector<adc_oneshot_unit_handle_t> ANALOG_UNITS { 2 };

    const InternalPinPtr pin;
    const adc_channel_t channel;
    adc_oneshot_unit_handle_t handle;
    adc_cali_handle_t caliHandle;
};

}    // namespace cornucopia::ugly_duckling::kernel

namespace ArduinoJson {

using cornucopia::ugly_duckling::kernel::InternalPin;
using cornucopia::ugly_duckling::kernel::InternalPinPtr;
using cornucopia::ugly_duckling::kernel::Pin;
using cornucopia::ugly_duckling::kernel::PinPtr;

template <>
struct Converter<PinPtr> {
    static void toJson(const PinPtr& src, JsonVariant dst) {
        if (src == nullptr) {
            dst.set(nullptr);
        } else {
            dst.set(src->getName());
        }
    }

    // nullptr for an unknown pin name; PropertyValidator<PinPtr> reports it
    static PinPtr fromJson(JsonVariantConst src) {
        return Pin::byName(src.as<const char*>());
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

    // nullptr for an unknown pin name; PropertyValidator<InternalPinPtr> reports it
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

namespace cornucopia::ugly_duckling::kernel::config {

template <>
struct PropertyValidator<PinPtr> {
    static Result<> validate(const PinPtr& pin, JsonVariantConst json) {
        if (pin == nullptr) {
            return failure("Unknown pin: " + json.as<std::string>());
        }
        return {};
    }
};

template <>
struct PropertyValidator<InternalPinPtr> {
    static Result<> validate(const InternalPinPtr& pin, JsonVariantConst json) {
        if (pin == nullptr) {
            return failure("Unknown internal pin: " + json.as<std::string>());
        }
        return {};
    }
};

}    // namespace cornucopia::ugly_duckling::kernel::config
