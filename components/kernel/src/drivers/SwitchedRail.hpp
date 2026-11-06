#pragma once

/*
 * A supply rail for external sensors that is only powered while someone needs it.
 *
 * On MK14 the I2C port and the RS485 transceiver run off +3V3_BUS, a load switch on LOADEN, the
 * same pin that wakes the H-bridge and the VLOAD boost. Earlier boards power their sensors all the
 * time, and use SwitchedRail::alwaysOn().
 *
 * Pure C++: no ESP-IDF headers, so drivers built on it compile in native unit tests.
 */

#include <Result.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace cornucopia::ugly_duckling::kernel::drivers {

/**
 * @brief What turns a SwitchedRail on and off.
 */
struct IRailSwitch {
    virtual ~IRailSwitch() = default;

    /**
     * @brief Turn the rail on, and return once it has settled; idempotent.
     */
    virtual void acquire() = 0;

    /**
     * @brief Allow the rail to turn off; idempotent.
     */
    virtual void release() = 0;

    /**
     * @brief Changes every time the rail turns on, i.e. whenever whatever it powers has been power-cycled.
     */
    virtual uint32_t getActivationCount() const = 0;

    /**
     * @brief When the rail last turned on; meaningful only while it is on.
     */
    virtual std::chrono::steady_clock::time_point getActivatedAt() const = 0;
};

class SwitchedRail : public std::enable_shared_from_this<SwitchedRail> {
public:
    using Delay = std::function<void(std::chrono::microseconds)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    /**
     * @brief A rail that is powered all the time: sessions never wait, and the activation never changes.
     */
    static std::shared_ptr<SwitchedRail> alwaysOn() {
        return std::shared_ptr<SwitchedRail>(new SwitchedRail(nullptr, {}, {}));
    }

    /**
     * @param delay Blocks the calling task for at least the given time.
     */
    static std::shared_ptr<SwitchedRail> switchedBy(
        std::shared_ptr<IRailSwitch> railSwitch,
        Delay delay,
        Clock clock = std::chrono::steady_clock::now) {
        return std::shared_ptr<SwitchedRail>(new SwitchedRail(std::move(railSwitch), std::move(delay), std::move(clock)));
    }

    /**
     * @brief Keeps the rail powered while it lives.
     */
    class Session {
    public:
        Session(Session&& other) noexcept
            : rail(std::move(other.rail))
            , activation(other.activation) {
        }

        Session& operator=(Session&& other) noexcept {
            if (this != &other) {
                end();
                rail = std::move(other.rail);
                activation = other.activation;
            }
            return *this;
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        ~Session() {
            end();
        }

        /**
         * @brief Identifies the power-on this session runs in: when it differs from an earlier session's,
         * whatever is on the rail has been power-cycled in between, and lost its state.
         */
        uint32_t getActivation() const {
            return activation;
        }

        /**
         * @brief Wait until the rail has been on for at least `duration`, e.g. for a sensor to start up.
         *
         * @details Time the rail has already spent on, e.g. for another client, counts towards it.
         */
        void waitPoweredFor(std::chrono::microseconds duration) const {
            rail->waitPoweredFor(duration);
        }

    private:
        friend class SwitchedRail;

        Session(std::shared_ptr<SwitchedRail> rail, uint32_t activation)
            : rail(std::move(rail))
            , activation(activation) {
        }

        void end() {
            if (rail) {
                rail->endSession();
                rail.reset();
            }
        }

        std::shared_ptr<SwitchedRail> rail;
        uint32_t activation;
    };

    /**
     * @brief Keeps the rail powered between sessions while it lives, without turning it on by itself.
     *
     * @details Lets a series of reads, e.g. one telemetry pass, share one power-up when there is
     * anything to read, and costs nothing when there is not.
     */
    class Batch {
    public:
        Batch(Batch&& other) noexcept
            : rail(std::move(other.rail)) {
        }

        Batch& operator=(Batch&& other) noexcept {
            if (this != &other) {
                end();
                rail = std::move(other.rail);
            }
            return *this;
        }

        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;

        ~Batch() {
            end();
        }

    private:
        friend class SwitchedRail;

        explicit Batch(std::shared_ptr<SwitchedRail> rail)
            : rail(std::move(rail)) {
        }

        void end() {
            if (rail) {
                rail->endBatch();
                rail.reset();
            }
        }

        std::shared_ptr<SwitchedRail> rail;
    };

    /**
     * @brief Power the rail, and return once it has settled.
     */
    Session acquire() {
        if (!railSwitch) {
            return { shared_from_this(), 0 };
        }
        {
            std::scoped_lock lock(mutex);
            sessions++;
            held = true;
        }
        // IRailSwitch::acquire() is idempotent, but concurrent callers must not race past each other
        // before the rail has actually turned on
        std::scoped_lock lock(acquireMutex);
        railSwitch->acquire();
        return { shared_from_this(), railSwitch->getActivationCount() };
    }

    Batch batch() {
        if (railSwitch) {
            std::scoped_lock lock(mutex);
            batches++;
        }
        return Batch(shared_from_this());
    }

    bool isSwitched() const {
        return railSwitch != nullptr;
    }

private:
    SwitchedRail(std::shared_ptr<IRailSwitch> railSwitch, Delay delay, Clock clock)
        : railSwitch(std::move(railSwitch))
        , delay(std::move(delay))
        , clock(std::move(clock)) {
    }

    void waitPoweredFor(std::chrono::microseconds duration) const {
        if (!railSwitch) {
            return;
        }
        auto poweredUntil = railSwitch->getActivatedAt() + duration;
        auto now = clock();
        if (now < poweredUntil) {
            delay(std::chrono::ceil<std::chrono::microseconds>(poweredUntil - now));
        }
    }

    void endSession() {
        if (!railSwitch) {
            return;
        }
        std::scoped_lock lock(mutex);
        sessions--;
        releaseIfUnused();
    }

    void endBatch() {
        if (!railSwitch) {
            return;
        }
        std::scoped_lock lock(mutex);
        batches--;
        releaseIfUnused();
    }

    // Must be called with `mutex` held
    void releaseIfUnused() {
        if (held && sessions == 0 && batches == 0) {
            held = false;
            railSwitch->release();
        }
    }

    const std::shared_ptr<IRailSwitch> railSwitch;
    const Delay delay;
    const Clock clock;

    std::mutex mutex;
    int sessions = 0;
    int batches = 0;
    // Whether we have acquired the switch: a batch keeps it after the last session ends
    bool held = false;

    std::mutex acquireMutex;
};

/**
 * @brief For a RailDevice whose device keeps no settings we'd have to restore after a power-up.
 */
struct NoPowerOnSetup { };
inline constexpr NoPowerOnSetup NO_POWER_ON_SETUP {};

/**
 * @brief A device on a switched rail that loses its settings whenever the rail turns off.
 *
 * @details Owns `T`, whatever the driver talks to the device through (an I²C descriptor, a bus
 * wrapper, a chip driver), and hands it out only through power(). So a driver cannot reach the
 * device without powering it: a driver that would work on a board with an always-on rail but not
 * on MK14 doesn't compile.
 */
template <typename T>
class RailDevice {
public:
    using PowerOnSetup = std::function<Result<>(T&)>;

    /**
     * @brief Access to the powered device; keeps the rail on while it lives.
     */
    class Powered {
    public:
        T& operator*() const {
            return *device;
        }

        /**
         * @brief Reaches through a smart pointer, so a device held as e.g. `std::shared_ptr<I2CDevice>`
         * reads `powered->readReg(...)`.
         */
        auto operator->() const {
            if constexpr (requires { device->operator->(); }) {
                return device->operator->();
            } else {
                return device;
            }
        }

    private:
        friend class RailDevice;

        Powered(SwitchedRail::Session session, T* device)
            : session(std::move(session))
            , device(device) {
        }

        SwitchedRail::Session session;
        T* device;
    };

    /**
     * @param startupTime How long the device needs after power-on before it responds.
     * @param powerOnSetup Sets the device up after a power-on; also run once on a rail that is always on.
     * @param args Constructs the `T` that the driver talks to the device through.
     */
    template <typename... Args>
    RailDevice(std::shared_ptr<SwitchedRail> rail, std::chrono::microseconds startupTime, PowerOnSetup powerOnSetup, Args&&... args)
        : rail(std::move(rail))
        , startupTime(startupTime)
        , powerOnSetup(std::move(powerOnSetup))
        , device(std::forward<Args>(args)...) {
    }

    /**
     * @brief A device with nothing to set up after a power-up; it only needs its start-up time.
     */
    template <typename... Args>
    RailDevice(std::shared_ptr<SwitchedRail> rail, std::chrono::microseconds startupTime, NoPowerOnSetup /*unused*/, Args&&... args)
        : RailDevice(std::move(rail), startupTime, PowerOnSetup {}, std::forward<Args>(args)...) {
    }

    /**
     * @brief Power the device, wait for it to start up, and set it up again if it lost power since it
     * was last set up.
     */
    Result<Powered> power() {
        auto session = rail->acquire();
        session.waitPoweredFor(startupTime);
        std::scoped_lock lock(mutex);
        if (powerOnSetup && setUpActivation != session.getActivation()) {
            TRY(powerOnSetup(device));
            setUpActivation = session.getActivation();
        }
        return Powered(std::move(session), &device);
    }

    /**
     * @brief Power the device and set it up, e.g. while creating it, to find out early if it does not respond.
     */
    Result<> initialize() {
        TRY(power());
        return {};
    }

    /**
     * @brief Access the device without powering it, only for what doesn't talk to it: setting up or
     * freeing the driver's own descriptor.
     */
    T& unpowered() {
        return device;
    }

private:
    const std::shared_ptr<SwitchedRail> rail;
    const std::chrono::microseconds startupTime;
    const PowerOnSetup powerOnSetup;
    T device;

    std::mutex mutex;
    std::optional<uint32_t> setUpActivation;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers
