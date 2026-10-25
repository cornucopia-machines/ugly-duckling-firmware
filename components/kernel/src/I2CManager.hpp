#pragma once
#include <EspError.hpp>
#include <Pin.hpp>
#include <Result.hpp>
#include <Strings.hpp>

#include <driver/i2c_master.h>
#include <i2cdev.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cornucopia::ugly_duckling::kernel {

using cornucopia::ugly_duckling::kernel::PinPtr;

using GpioPair = std::pair<PinPtr, PinPtr>;

LOGGING_TAG(I2C, "i2c")

struct I2CConfig {
public:
    uint8_t address;
    InternalPinPtr sda;
    InternalPinPtr scl;
    uint32_t clkSpeed = 400000;    // Hz; set by I2CSettings (which takes kHz) or overridden per-driver

    std::string toString() const {
        return "I2C address: " + toHexString(address) + ", SDA: " + sda->getName() + ", SCL: " + scl->getName() + ", clk: " + std::to_string(clkSpeed / 1000) + " kHz";
    }
};

struct I2CBus {
    const i2c_port_t port;
    const InternalPinPtr sda;
    const InternalPinPtr scl;
};

class I2CDevice {
public:
    I2CDevice(const std::string& name, const std::shared_ptr<I2CBus>& bus, uint8_t address, uint32_t clkSpeed = 400000)
        : name(name)
        , bus(bus)
        , device({
              .port = bus->port,
              .addr = address,
              .addr_bit_len = I2C_ADDR_BIT_LEN_7,
              .mutex = nullptr,         // Will be created in the constructor
              .dev_handle = nullptr,    // Populated after init
              .sda_pin = 0,             // Populated after init
              .scl_pin = 0,             // Populated after init
              .timeout_ticks = 0,       // Use default timeout
              .cfg = {
                  .sda_io_num = bus->sda->getGpio(),
                  .scl_io_num = bus->scl->getGpio(),
                  // Note: These enable ~45kOhm pull-ups; we still need stronger external ones
                  //       for proper operation (~4.7kOhm, or even lower).
                  .sda_pullup_en = 1,
                  .scl_pullup_en = 1,
                  .clk_flags = 0,    // Use default clock flags
                  .master {
                      .clk_speed = clkSpeed,
                  },
              },
          }) {
        // TODO Do we need a mutex here?
        i2c_dev_create_mutex(&device);
    }

    ~I2CDevice() {
        i2c_dev_delete_mutex(&device);
    }

    esp_err_t probeRead() {
        return i2c_dev_check_present(&device);
    }

    Result<uint8_t> readRegByte(uint8_t reg) {
        uint8_t value;
        ESP_TRY(i2c_dev_read(&device, &reg, 1, &value, 1));
        return value;
    }

    Result<uint16_t> readRegWord(uint8_t reg) {
        uint16_t value;
        ESP_TRY(i2c_dev_read(&device, &reg, 1, &value, 2));
        return value;
    }

    Result<> readReg(uint8_t reg, uint8_t* buffer, size_t length) {
        return espResult(i2c_dev_read(&device, &reg, 1, buffer, length));
    }

    Result<> writeRegByte(uint8_t reg, uint8_t value) {
        return espResult(i2c_dev_write(&device, &reg, 1, &value, 1));
    }

    Result<> writeRegWord(uint8_t reg, uint16_t value) {
        return espResult(i2c_dev_write(&device, &reg, 1, &value, 2));
    }

    Result<> writeReg(uint8_t reg, uint8_t* buffer, size_t length) {
        return espResult(i2c_dev_write(&device, &reg, 1, buffer, length));
    }

    Result<> writeByte(uint8_t cmd) {
        return espResult(i2c_dev_write(&device, &cmd, 1, nullptr, 0));
    }

    Result<std::vector<uint8_t>> readBytes(uint8_t cmd, size_t n) {
        std::vector<uint8_t> buffer(n);
        ESP_TRY(i2c_dev_read(&device, &cmd, 1, buffer.data(), n));
        return buffer;
    }

    std::shared_ptr<I2CBus> getBus() const {
        return bus;
    }

    uint16_t getAddress() const {
        return device.addr;
    }

private:
    const std::string name;
    const std::shared_ptr<I2CBus> bus;
    i2c_dev_t device;
};

class I2CManager {
public:
    I2CManager() {
        ESP_ERROR_CHECK(i2cdev_init());
        buses.reserve(I2C_NUM_MAX);
    }

    ~I2CManager() {
        ESP_ERROR_CHECK(i2cdev_done());
    }

    Result<std::shared_ptr<I2CDevice>> createDevice(const std::string& name, const I2CConfig& config) {
        auto bus = TRY(getBusFor(config.sda, config.scl));
        auto device = std::make_shared<I2CDevice>(name, bus, config.address, config.clkSpeed);
        LOGTI(I2C, "Created I2C device %s at address 0x%02x (clk: %.02f kHz)",
            name.c_str(), config.address, config.clkSpeed / 1000.0);
        return device;
    }

    Result<std::shared_ptr<I2CDevice>> createDevice(const std::string& name, const InternalPinPtr& sda, const InternalPinPtr& scl, uint8_t address) {
        auto bus = TRY(getBusFor(sda, scl));
        auto device = std::make_shared<I2CDevice>(name, bus, address);
        LOGTI(I2C, "Created I2C device %s at address 0x%02x",
            name.c_str(), address);
        // Test if communication is possible
        // esp_err_t err = device->probeRead();
        // if (err != ESP_OK) {
        //     return failure(
        //         "Failed to communicate with I2C device " + name + " at address 0x" + std::to_string(address) + ": " + esp_err_to_name(err);
        // }
        return device;
    }

    Result<std::shared_ptr<I2CBus>> getBusFor(const I2CConfig& config) {
        return getBusFor(config.sda, config.scl);
    }

    Result<std::shared_ptr<I2CBus>> getBusFor(const InternalPinPtr& sda, const InternalPinPtr& scl) {
        std::scoped_lock lock(mutex);
        for (auto bus : buses) {
            if (bus->sda == sda && bus->scl == scl) {
                LOGTV(I2C, "Using previously registered I2C bus #%d for SDA: %s, SCL: %s",
                    static_cast<int>(bus->port), sda->getName().c_str(), scl->getName().c_str());
                return bus;
            }
        }
        auto port = TRY(selectPort(sda, scl));
        LOGTI(I2C, "Registering I2C bus #%d for SDA: %s, SCL: %s",
            static_cast<int>(port), sda->getName().c_str(), scl->getName().c_str());
        auto bus = std::make_shared<I2CBus>(I2CBus { .port = port, .sda = sda, .scl = scl });
        buses.push_back(bus);
        return bus;
    }

private:
    std::mutex mutex;
    std::vector<std::shared_ptr<I2CBus>> buses;

    // On chips with LP_I2C (ESP32-C6): LP_I2C_NUM_0 is pin-locked to GPIO6/GPIO7; the single HP bus is I2C_NUM_0.
    // On other platforms (ESP32-S3): all buses are HP, assigned in registration order.
    Result<i2c_port_t> selectPort(const InternalPinPtr& sda, const InternalPinPtr& scl) {
#if SOC_LP_I2C_SUPPORTED
        if (sda->getGpio() == GPIO_NUM_6 && scl->getGpio() == GPIO_NUM_7) {
            LOGTI(I2C, "Using LP I2C bus #%d for SDA: %s, SCL: %s",
                static_cast<int>(LP_I2C_NUM_0), sda->getName().c_str(), scl->getName().c_str());
            return LP_I2C_NUM_0;
        }
        for (const auto& b : buses) {
            if (b->port == I2C_NUM_0) {
                return failure("Maximum number of I2C buses reached");
            }
        }
        LOGTI(I2C, "Using HP I2C bus #%d for SDA: %s, SCL: %s",
            static_cast<int>(I2C_NUM_0), sda->getName().c_str(), scl->getName().c_str());
        return I2C_NUM_0;
#else
        if (buses.size() >= SOC_HP_I2C_NUM) {
            return failure("Maximum number of I2C buses reached");
        }
        return static_cast<i2c_port_t>(buses.size());
#endif
    }
};

}    // namespace cornucopia::ugly_duckling::kernel
