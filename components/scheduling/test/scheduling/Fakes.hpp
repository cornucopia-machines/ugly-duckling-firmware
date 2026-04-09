// fakes.hpp
#pragma once

#include <cmath>
#include <deque>

#include <peripherals/api/IFlowMeter.hpp>
#include <peripherals/api/ISoilMoistureSensor.hpp>

#include <scheduling/MoistureBasedScheduler.hpp>

using namespace cornucopia::ugly_duckling::peripherals::api;

namespace cornucopia::ugly_duckling::utils::scheduling {

struct FakeClock {
    ms time { 0 };
    [[nodiscard]] ms now() const {
        return time;
    }
    void advance(ms dt) {
        time += dt;
    }
};

struct FakePeripheral : virtual IPeripheral {

    FakePeripheral(std::string name)
        : name(std::move(name)) {
    }

    const std::string& getName() const override {
        return name;
    }

    const std::string name;
};

struct FakeFlowMeter : FakePeripheral, IFlowMeter {
    FakeFlowMeter()
        : FakePeripheral("flow-meter") {
    }

    Liters bucket { 0.0 };
    Liters getVolume() override {
        auto r = bucket;
        bucket = 0.0;
        return r;
    }
};

struct FakeSoilMoistureSensor : FakePeripheral, ISoilMoistureSensor {
    FakeSoilMoistureSensor()
        : FakePeripheral("soil-moisture-sensor") {
    }

    Percent moisture { 50.0 };
    Percent getMoisture() override {
        return moisture;
    }
};

// Simple FOPDT-ish soil simulator (test-only)
struct SoilSimulator {
    struct Config {
        // Each watering creates an impulse whose total integrated contribution (area) tends toward gainPercentPerLiter * volume (%).
        double gainPercentPerLiter;         // Integrated % moisture per liter (area under exponential after dead time)
        ms deadTime;                        // Transport delay before water affects moisture
        ms tau;                             // Decay time constant of the impulse (roughly effect halves every ~0.69*tau)
        double evaporationPercentPerMin;    // Natural linear evaporation (% per minute)
    };

    struct Input {
        ms time {};
        Liters volume {};
    };
    std::deque<Input> wateringHistory {};

    SoilSimulator(const Config& config)
        : config(config) {
    }

    void inject(ms now, Liters volume) {
        if (volume > 0.0) {
            wateringHistory.push_back({ now, volume });
        }
        // Trim very old inputs (after effect is negligible: ageAfterDead > 8*tau)
        while (!wateringHistory.empty() && (now - wateringHistory.front().time) > (config.deadTime + 8 * config.tau)) {
            wateringHistory.pop_front();
        }
    }

    // Advance one tick (discrete integration of exponential decays)
    void step(ms now, Percent& moisture, ms dt) const {
        if (dt <= 0ms) {
            return;    // nothing to do
        }

        // 1. Evaporation (linear approximation)
        const double dtInMin = static_cast<double>(dt.count()) / 60000.0;
        moisture = std::max(0.0, moisture - (config.evaporationPercentPerMin * dtInMin));

        // 2. Add contributions from each watering whose dead time has passed.
        //    Exact discrete integral over the interval [t, t+dt] of A * (1/tau) * exp(-age/tau) d(age)
        //    where A = gainPercentPerLiter * volume.
        const double tauMs = static_cast<double>(config.tau.count());
        const double dtMs = static_cast<double>(dt.count());
        const double expNegDtOverTau = std::exp(-dtMs / tauMs);
        double delta = 0.0;    // total percent to add this tick
        for (auto const& watering : wateringHistory) {
            auto effectStart = watering.time + config.deadTime;
            if (now <= effectStart) {
                continue;    // not started yet
            }
            // Age at current time (start of interval)
            double ageMs = static_cast<double>((now - effectStart).count());
            // Exponential factor at beginning of interval
            double expNegAge = std::exp(-ageMs / tauMs);
            // Increment = A * (exp(-age/tau) - exp(-(age+dt)/tau)) = A * exp(-age/tau) * (1 - exp(-dt/tau))
            double A = config.gainPercentPerLiter * watering.volume;    // total area (%) this impulse would add over infinite time
            double inc = A * expNegAge * (1.0 - expNegDtOverTau);
            delta += inc;
        }
        moisture = std::min(100.0, moisture + delta);
    }

    Config config;
};

}    // namespace cornucopia::ugly_duckling::utils::scheduling
