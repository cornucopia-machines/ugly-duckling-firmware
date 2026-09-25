#pragma once
#include <I2CManager.hpp>
#include <MovingAverage.hpp>
#include <Named.hpp>
#include <config/Configuration.hpp>
#include <peripherals/I2CSettings.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ILightSensor.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::peripherals;
using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::peripherals::light_sensor {

class LightSensor
    : public api::ILightSensor,
      public Peripheral {
public:
    LightSensor(
        const std::string& name,
        seconds measurementFrequency,
        seconds latencyInterval)
        : Peripheral(name)
        , measurementFrequency(measurementFrequency)
        , level(static_cast<std::size_t>(latencyInterval.count() / measurementFrequency.count())) {
    }

    Lux getLightLevel() override {
        std::scoped_lock lock(updateAverageMutex);
        return level.getAverage();
    }

    seconds getMeasurementFrequency() {
        return measurementFrequency;
    }

protected:
    virtual double readLightLevel() = 0;

    void runLoop() {
        Task::loop(name, 3072, [this](Task& task) {
            auto currentLevel = readLightLevel();
            {
                std::scoped_lock lock(updateAverageMutex);
                level.record(currentLevel);
            }
            task.delayUntil(measurementFrequency);
        });
    }

private:
    const seconds measurementFrequency;
    std::mutex updateAverageMutex;
    MovingAverage<double> level;
};

}    // namespace cornucopia::ugly_duckling::peripherals::light_sensor
