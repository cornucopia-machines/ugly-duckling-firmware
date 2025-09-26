#pragma once

#include <exception>
#include <memory>

#include <driver/i2c_master.h>

#include <i2cdev.h>

#include <Concurrent.hpp>
#include <EspException.hpp>
#include <Pin.hpp>
#include <Strings.hpp>
#include <utility>

namespace farmhub::kernel {

using farmhub::kernel::PinPtr;

using GpioPair = std::pair<PinPtr, PinPtr>;

LOGGING_TAG(I2C, "i2c")

struct I2CConfig {
public:
    uint8_t address;
    InternalPinPtr sda;
    InternalPinPtr scl;

    std::string toString() const {
        return "I2C address: 0x" + toHexString(address) + ", SDA: " + sda->getName() + ", SCL: " + scl->getName();
    }
};

struct I2CBus {
    /** Lookup the I2C bus handle if already allocated by i2c_bus_create() */
    i2c_master_bus_handle_t lookupHandle() const {
        i2c_master_bus_handle_t bus;
        ESP_ERROR_THROW(i2c_master_get_bus_handle(port, &bus));
        return bus;
    }

    const i2c_port_t port;
    const InternalPinPtr sda;
    const InternalPinPtr scl;
};

class I2CDevice {
public:
    I2CDevice(const std::string& name, const std::shared_ptr<I2CBus>& bus, uint8_t address)
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
                      // TODO Allow clock speed to be configured
                      //      And use higher speed for internal I2C
                      .clk_speed = 200000,
                  },
              },
          }) {
        // TODO Do we need a mutex here?
        i2c_dev_create_mutex(&device);
    }

    ~I2CDevice() {
        i2c_dev_delete_mutex(&device);
    }

    esp_err_t probeRead() const {
        return i2c_dev_check_present(&device);
    }

    uint8_t readRegByte(uint8_t reg) const {
        uint8_t value;
        ESP_ERROR_THROW(i2c_dev_read_reg(&device, reg, &value, 1));
        return value;
    }

    uint16_t readRegWord(uint8_t reg) const {
        uint16_t value;
        ESP_ERROR_THROW(i2c_dev_read_reg(&device, reg, &value, 2));
        // I2C uses big-endian
        return __builtin_bswap16(value);
    }

    void readReg(uint8_t reg, uint8_t* buffer, size_t length) const {
        ESP_ERROR_THROW(i2c_dev_read_reg(&device, reg, buffer, length));
    }

    void writeRegByte(uint8_t reg, uint8_t value) const {
        ESP_ERROR_THROW(i2c_dev_write_reg(&device, reg, &value, 1));
    }

    void writeRegWord(uint8_t reg, uint16_t value) const {
        // I2C uses big-endian
        auto valueBE = __builtin_bswap16(value);
        ESP_ERROR_THROW(i2c_dev_write_reg(&device, reg, &valueBE, 2));
    }

    void writeReg(uint8_t reg, uint8_t* buffer, size_t length) const {
        ESP_ERROR_THROW(i2c_dev_write_reg(&device, reg, buffer, length));
    }

    std::shared_ptr<I2CBus> getBus() const {
        return bus;
    }

    uint8_t getAddress() const {
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
        ESP_ERROR_THROW(i2cdev_init());
        buses.reserve(I2C_NUM_MAX);
    }

    ~I2CManager() {
        ESP_ERROR_CHECK(i2cdev_done());
    }

    std::shared_ptr<I2CDevice> createDevice(const std::string& name, const I2CConfig& config) {
        return createDevice(name, config.sda, config.scl, config.address);
    }

    std::shared_ptr<I2CDevice> createDevice(const std::string& name, const InternalPinPtr& sda, const InternalPinPtr& scl, uint8_t address) {
        auto device = std::make_shared<I2CDevice>(name, getBusFor(sda, scl), address);
        LOGTI(I2C, "Created I2C device %s at address 0x%02x",
            name.c_str(), address);
        // Test if communication is possible
        // esp_err_t err = device->probeRead();
        // if (err != ESP_OK) {
        //     throw std::runtime_error(
        //         "Failed to communicate with I2C device " + name + " at address 0x" + std::to_string(address) + ": " + esp_err_to_name(err);
        // }
        return device;
    }

    std::shared_ptr<I2CBus> getBusFor(const I2CConfig& config) {
        return getBusFor(config.sda, config.scl);
    }

    std::shared_ptr<I2CBus> getBusFor(const InternalPinPtr& sda, const InternalPinPtr& scl) {
        Lock lock(mutex);
        for (auto bus : buses) {
            if (bus->sda == sda && bus->scl == scl) {
                LOGTV(I2C, "Using previously registered I2C bus #%d for SDA: %s, SCL: %s",
                    static_cast<int>(bus->port), sda->getName().c_str(), scl->getName().c_str());
                return bus;
            }
        }
        auto nextBus = buses.size();
        if (nextBus < I2C_NUM_MAX) {
            LOGTI(I2C, "Registering I2C bus #%d for SDA: %s, SCL: %s",
                nextBus, sda->getName().c_str(), scl->getName().c_str());
            auto bus = std::make_shared<I2CBus>(I2CBus { .port = static_cast<i2c_port_t>(nextBus), .sda = sda, .scl = scl });
            buses.push_back(bus);
            return bus;
        }

        throw std::runtime_error("Maximum number of I2C buses reached");
    }

private:
    Mutex mutex;
    std::vector<std::shared_ptr<I2CBus>> buses;
};

}    // namespace farmhub::kernel
