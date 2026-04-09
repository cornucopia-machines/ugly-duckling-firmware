#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace cornucopia::ugly_duckling::utils::scheduling {
/**
 * @brief Kalman filter to estimate true soil moisture and temperature sensitivity.
 *
 * State vector = [moistReal, beta]^T
 * Measurement model: moistObserved = moistReal + beta * (temp - tempRef) + noise
 *
 * Higher Process Noise (Q): The filter adapts more quickly to changes in the measurements,
 *     as it assumes the system state could be changing significantly. This can make the
 *     estimated state more responsive but also more susceptible to noisy measurements.
 *
 * Higher Measurement Noise (R): The filter smooths the measurements more, as it assumes
 *     the measurements are unreliable. This results in a smoother estimated state but can
 *     make the filter slower to react to actual changes in the system state.
 */
class MoistureKalmanFilter {
public:
    MoistureKalmanFilter(double initMoistReal = 0.0,
        double initBeta = 0.0,
        double tempRef = 20.0)
        : moistReal(initMoistReal)
        , beta(initBeta)
        , tempRef(tempRef) {
        // Initialize covariance as moderately uncertain
        P = { { { 1.0, 0.0 }, { 0.0, 1.0 } } };
    }

    /// @brief Update filter with new observation
    /// @param moistObserved measured soil moisture (sensor)
    /// @param temp measured soil temperature
    /// @param qMoist process noise for moistReal (slow drift, higher during watering)
    /// @param qBeta process noise for beta (usually tiny)
    /// @param R measurement noise variance
    void update(double moistObserved,
        double temp,
        double qMoist,
        double qBeta,
        double R) {

        // Predict step: state does not change (identity transition)
        // Add process noise
        P[0][0] += qMoist;
        P[1][1] += qBeta;

        // Observation model: H = [1, temp - tempRef]
        const double h0 = 1.0;
        const double h1 = (temp - tempRef);

        // predicted measurement
        const double moistPred = moistReal + (beta * h1);
        const double innovation = moistObserved - moistPred;

        // Innovation covariance: S = H P H^T + R
        double S = (P[0][0] * h0 * h0) + (2.0 * P[0][1] * h0 * h1) + (P[1][1] * h1 * h1) + R;

        // Guard against pathological collapse
        const double sFloor = 1e-12;
        S = std::max(S, sFloor);

        // Kalman gain K = P H^T / S
        double K0 = (P[0][0] * h0 + P[0][1] * h1) / S;
        double K1 = (P[1][0] * h0 + P[1][1] * h1) / S;

        // Cap gain a bit for robustness
        constexpr double kCap = 1e6;
        K0 = std::clamp(K0, -kCap, kCap);
        K1 = std::clamp(K1, -kCap, kCap);

        // Update state
        moistReal += K0 * innovation;
        beta += K1 * innovation;

        // Update covariance (Joseph form)
        // A = I - K H
        const double a00 = 1.0 - (K0 * h0);
        const double a01 = -K0 * h1;
        const double a10 = -K1 * h0;
        const double a11 = 1.0 - (K1 * h1);

        // A * P
        double AP00 = (a00 * P[0][0]) + (a01 * P[1][0]);
        double AP01 = (a00 * P[0][1]) + (a01 * P[1][1]);
        double AP10 = (a10 * P[0][0]) + (a11 * P[1][0]);
        double AP11 = (a10 * P[0][1]) + (a11 * P[1][1]);

        // P_new = (A P) A^T + K R K^T
        double Pnew00 = (AP00 * a00) + (AP01 * a01) + (K0 * R * K0);
        double Pnew01 = (AP00 * a10) + (AP01 * a11) + (K0 * R * K1);
        double Pnew10 = (AP10 * a00) + (AP11 * a01) + (K1 * R * K0);
        double Pnew11 = (AP10 * a10) + (AP11 * a11) + (K1 * R * K1);

        // Enforce symmetry (numerical hygiene)
        const double sym01 = 0.5 * (Pnew01 + Pnew10);
        // Tiny floor instead of 0 to avoid singular collapse
        constexpr double pFloor = 1e-15;

        P[0][0] = std::max(Pnew00, pFloor);
        P[0][1] = sym01;
        P[1][0] = sym01;
        P[1][1] = std::max(Pnew11, pFloor);
    }

    [[nodiscard]] double getMoistReal() const noexcept {
        return moistReal;
    }
    [[nodiscard]] double getBeta() const noexcept {
        return beta;
    }
    [[nodiscard]] double getTempRef() const noexcept {
        return tempRef;
    }

    void setTempRef(double newRef) noexcept {
        tempRef = newRef;
    }

private:
    double moistReal;    // calculated moisture at tempRef
    double beta;         // d(moist)/d°C sensitivity
    double tempRef;      // reference temperature

    // 2x2 covariance matrix
    std::array<std::array<double, 2>, 2> P {};
};

}    // namespace cornucopia::ugly_duckling::utils::scheduling
