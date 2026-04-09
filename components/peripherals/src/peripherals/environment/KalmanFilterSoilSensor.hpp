#pragma once

#include <chrono>
#include <memory>

#include <Configuration.hpp>
#include <peripherals/Peripheral.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>
#include <peripherals/api/ITemperatureSensor.hpp>

#include <scheduling/MoistureKalmanFilter.hpp>

#include "Environment.hpp"

using namespace std::chrono;
using namespace cornucopia::ugly_duckling::utils::scheduling;

namespace cornucopia::ugly_duckling::peripherals::environment {

/**
 * @brief Reports real soil moisture levels calculated from raw moisture and temperature data using a Kalman filter.
 */
class KalmanFilterSoilSensorSettings
    : public ConfigurationSection {
public:
    Property<std::string> rawMoistureSensor { this, "rawMoistureSensor" };
    Property<std::string> temperatureSensor { this, "temperatureSensor" };

    Property<Percent> initialMoisture { this, "initialMoisture", 50.0 };
    Property<double> initialBeta { this, "initialBeta", 0.0 };
    Property<Celsius> tempRef { this, "tempRef", 20.0 };

    // Process noise
    Property<double> qMoist { this, "qMoist", 1e-5 };
    Property<double> qBeta { this, "qBeta", 1e-6 };

    // Measurement noise
    Property<double> rSensitive { this, "rSensitive", 1e-3 };
    Property<double> rNormal { this, "rNormal", 1e-1 };

    // Period at start to use sensitive R value to allow quick convergence
    Property<seconds> sensitivePeriod { this, "sensitivePeriod", 15min };
};

class KalmanFilterSoilSensor
    : public api::ISoilMoistureSensor,
      public Peripheral {
public:
    KalmanFilterSoilSensor(
        const std::string& name,
        const std::shared_ptr<api::ISoilMoistureSensor>& rawMoistureSensor,
        const std::shared_ptr<api::ITemperatureSensor>& tempSensor,
        Percent initialMoisture,
        double initialBeta,
        Celsius tempRef,
        double qMoist,
        double qBeta,
        double rSensitive,
        double rNormal,
        seconds sensitivePeriod)
        : Peripheral(name)
        , kalmanFilter(initialMoisture, initialBeta, tempRef)
        , rawMoistureSensor(rawMoistureSensor)
        , tempSensor(tempSensor)
        , qMoist(qMoist)
        , qBeta(qBeta)
        , rSensitive(rSensitive)
        , rNormal(rNormal)
        , sensitivePeriodEnd(steady_clock::now() + sensitivePeriod) {
        LOGTI(ENV, "Initializing Kalman filter soil moisture sensor '%s' "
             "wrapping moisture sensor '%s'"
             " and temperature sensor '%s'"
             "; initial moisture: %.1f%%"
             ", initial beta: %.2f"
             ", reference temp.: %.1f C"
             ", process noise: %.2e (moisture) / %.2e (beta)"
             ", measurement noise: %.2e (sensitive) / %.2e (normal)"
             ", sensitive period: %lld s",
            name.c_str(),
            rawMoistureSensor->getName().c_str(),
            tempSensor->getName().c_str(),
            initialMoisture,
            initialBeta,
            tempRef,
            qMoist, qBeta,
            rSensitive, rNormal,
            duration_cast<seconds>(sensitivePeriod).count());
    }

    Percent getMoisture() override {
        auto rawMoisture = rawMoistureSensor->getMoisture();
        if (std::isnan(rawMoisture)) {
            LOGTW(ENV, "Raw moisture reading is NaN");
            return NAN;
        }
        auto temp = tempSensor->getTemperature();
        if (std::isnan(temp)) {
            LOGTW(ENV, "Temperature reading is NaN");
            return NAN;
        }

        auto r = (steady_clock::now() < sensitivePeriodEnd) ? rSensitive : rNormal;
        kalmanFilter.update(rawMoisture, temp, qMoist, qBeta, r);
        auto realMoisture = kalmanFilter.getMoistReal();
        LOGTV(ENV, "Updated Kalman filter with raw moisture: %.1f%%, temperature: %.1f C, real moisture: %.1f%%, beta: %.2f %%/C",
            rawMoisture, temp, realMoisture, kalmanFilter.getBeta());
        return realMoisture;
    }

    double getBeta() {
        return kalmanFilter.getBeta();
    }

private:
    MoistureKalmanFilter kalmanFilter;
    std::shared_ptr<api::ISoilMoistureSensor> rawMoistureSensor;
    std::shared_ptr<api::ITemperatureSensor> tempSensor;

    double qMoist;
    double qBeta;
    double rSensitive;
    double rNormal;
    std::chrono::steady_clock::time_point sensitivePeriodEnd;
};

inline PeripheralFactory makeFactoryForKalmanSoilMoisture() {
    return makePeripheralFactory<ISoilMoistureSensor, KalmanFilterSoilSensor, KalmanFilterSoilSensorSettings>(
        "environment:kalman-soil-moisture",
        "environment",
        [](PeripheralInitParameters& params, const std::shared_ptr<KalmanFilterSoilSensorSettings>& settings) {
            auto rawMoistureSensor = params.peripheral<api::ISoilMoistureSensor>(settings->rawMoistureSensor.get());
            auto tempSensor = params.peripheral<api::ITemperatureSensor>(settings->temperatureSensor.get());
            auto sensor = std::make_shared<KalmanFilterSoilSensor>(
                params.name,
                rawMoistureSensor,
                tempSensor,
                settings->initialMoisture.get(),
                settings->initialBeta.get(),
                settings->tempRef.get(),
                settings->qMoist.get(),
                settings->qBeta.get(),
                settings->rSensitive.get(),
                settings->rNormal.get(),
                settings->sensitivePeriod.get());
            params.registerFeature("moisture", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getMoisture();
            });
            params.registerFeature("kalman-beta", [sensor](JsonObject& telemetryJson) {
                telemetryJson["value"] = sensor->getBeta();
            });
            return sensor;
        });
}

}    // namespace cornucopia::ugly_duckling::peripherals::environment
