#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <random>

#include <scheduling/MoistureKalmanFilter.hpp>

#include <FakeLog.hpp>

using Catch::Approx;

namespace cornucopia::ugly_duckling::utils::scheduling {

struct SimulationConfig {
    double moistReal0 = 80.0;     // initial real moisture
    double betaReal = 0.20;       // real temp sensitivity (units of moist per °C)
    double tempMean = 20.0;       // °C
    double tempAmp = 3.0;         // diurnal swing amplitude
    double measNoiseSD = 0.02;    // measurement noise SD
};

struct SimulationResult {
    double temp;
    double moistReal;
    double moistObserved;
};

SimulationResult stepSim(const SimulationConfig& cfg,
    double t,
    double moistRealPrev,
    double tempRef,
    std::mt19937& rng) {
    // Temperature: slow sinusoidal variation
    double temp = cfg.tempMean + cfg.tempAmp * std::sin(t / 10.0);

    // Real moisture: default = no change (idle)
    double moistReal = moistRealPrev;

    // Observed moisture = real + beta*(temp - tempRef) + noise
    std::normal_distribution<double> noise(0.0, cfg.measNoiseSD);
    double moistObserved = moistReal + cfg.betaReal * (temp - tempRef) + noise(rng);

    return { temp, moistReal, moistObserved };
}

TEST_CASE("Kalman converges to real beta and moisture in idle", "[kalman][convergence]") {
    SimulationConfig cfg;
    const double tempRef = 20.0;

    // Filter starts a bit off
    MoistureKalmanFilter filter(/*initMoistReal*/ cfg.moistReal0 / 4.0,
        /*initBeta*/ 0.00,
        /*tempRef*/ tempRef);

    // Noise settings
    const double qMoistIdle = 1e-5;
    const double qBeta = 1e-6;
    const double R = cfg.measNoiseSD * cfg.measNoiseSD;

    std::mt19937 rng(12345);
    double moistReal = cfg.moistReal0;

    // Run for a while to converge
    for (int t = 0; t < 800; ++t) {
        auto s = stepSim(cfg, t, moistReal, tempRef, rng);
        filter.update(s.moistObserved, s.temp, qMoistIdle, qBeta, R);
    }

    // Expect beta close to truth and moistReal close to real level
    REQUIRE(filter.getBeta() == Approx(cfg.betaReal).margin(0.01));
    REQUIRE(filter.getMoistReal() == Approx(cfg.moistReal0).margin(0.20));
}

TEST_CASE("Idle stability: low qMoist yields a steady moistReal", "[kalman][stability]") {
    SimulationConfig cfg;
    const double tempRef = 20.0;
    MoistureKalmanFilter filter(cfg.moistReal0, 0.0, tempRef);

    std::mt19937 rng(7);
    const double qMoistIdle = 1e-6;
    const double qBeta = 1e-6;
    const double R = cfg.measNoiseSD * cfg.measNoiseSD;

    double moistReal = cfg.moistReal0;
    double minEst = 1e9, maxEst = -1e9;

    for (int t = 0; t < 600; ++t) {
        auto s = stepSim(cfg, t, moistReal, tempRef, rng);
        filter.update(s.moistObserved, s.temp, qMoistIdle, qBeta, R);
        minEst = std::min(minEst, filter.getMoistReal());
        maxEst = std::max(maxEst, filter.getMoistReal());
    }

    // Should be very steady (range small compared to noise & temp wobble)
    REQUIRE((maxEst - minEst) < 0.6);    // tune threshold as needed
}

TEST_CASE("Watering event: bump qMoist to let moistReal jump quickly", "[kalman][watering]") {
    SimulationConfig cfg;
    const double tempRef = 20.0;
    MoistureKalmanFilter filter(cfg.moistReal0, 0.0, tempRef);

    std::mt19937 rng(42);
    const double qMoistIdle = 1e-6;
    const double qMoistWater = 1e-2;    // much larger during watering
    const double qBeta = 1e-6;
    const double R = cfg.measNoiseSD * cfg.measNoiseSD;

    double moistReal = cfg.moistReal0;

    // Idle for a while
    for (int t = 0; t < 200; ++t) {
        auto s = stepSim(cfg, t, moistReal, tempRef, rng);
        filter.update(s.moistObserved, s.temp, qMoistIdle, qBeta, R);
    }

    // Watering jump: +5 units real moisture, let filter react with high qMoist
    moistReal += 5.0;
    for (int t = 200; t < 220; ++t) {
        auto s = stepSim(cfg, t, moistReal, tempRef, rng);
        filter.update(s.moistObserved, s.temp, qMoistWater, qBeta, R);
    }

    // After a short period, estimate should be close to new real level
    REQUIRE(filter.getMoistReal() == Approx(moistReal).margin(0.8));

    // Back to idle, it should stay near the new baseline
    for (int t = 220; t < 420; ++t) {
        auto s = stepSim(cfg, t, moistReal, tempRef, rng);
        filter.update(s.moistObserved, s.temp, qMoistIdle, qBeta, R);
    }
    REQUIRE(filter.getMoistReal() == Approx(moistReal).margin(0.5));
}

TEST_CASE("tempRef usage: consistent estimates around chosen reference", "[kalman][tempRef]") {
    SimulationConfig cfg;
    std::mt19937 rngA(1), rngB(1);    // same seed for comparable noise

    // Filter 1: tempRef = 20 °C
    MoistureKalmanFilter filterAt20C(cfg.moistReal0, 0.0, /*tempRef*/ 20.0);
    // Filter 2: tempRef = 0 °C
    MoistureKalmanFilter filterAt0C(cfg.moistReal0, 0.0, /*tempRef*/ 0.0);

    const double qMoist = 1e-5;
    const double qBeta = 1e-6;
    const double R = cfg.measNoiseSD * cfg.measNoiseSD;

    double moistRealA = cfg.moistReal0;
    double moistRealB = cfg.moistReal0;

    // Run parallel simulations; both should estimate the same *real* moisture,
    // even though beta values will differ by a constant offset due to tempRef.
    for (int t = 0; t < 800; ++t) {
        auto a = stepSim(cfg, t, moistRealA, /*tempRef for obs*/ 20.0, rngA);
        auto b = stepSim(cfg, t, moistRealB, /*tempRef for obs*/ 0.0, rngB);

        filterAt20C.update(a.moistObserved, a.temp, qMoist, qBeta, R);
        filterAt0C.update(b.moistObserved, b.temp, qMoist, qBeta, R);
    }

    // Moisture estimates should be close (within small tolerance)
    REQUIRE(filterAt20C.getMoistReal() == Approx(filterAt0C.getMoistReal()).margin(0.4));

    // Beta values will differ roughly by betaReal*(20.0 - 0.0) absorbed into moistReal,
    // so we don't compare beta directly here.
}

}    // namespace cornucopia::ugly_duckling::utils::scheduling
