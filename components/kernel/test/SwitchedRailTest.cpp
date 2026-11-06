#include <catch2/catch_test_macros.hpp>
#include <drivers/SwitchedRail.hpp>

#include <chrono>
#include <memory>
#include <optional>

using namespace std::chrono_literals;
using namespace cornucopia::ugly_duckling::kernel;
using namespace cornucopia::ugly_duckling::kernel::drivers;

namespace {

using TimePoint = std::chrono::steady_clock::time_point;

// Stands in for SharedEnable: other clients (e.g. the valve) can turn the rail on, too
struct FakeRailSwitch final : IRailSwitch {
    explicit FakeRailSwitch(TimePoint& now)
        : now(now) {
    }

    void acquire() override {
        if (!railHeld) {
            railHeld = true;
            turnOnIfNeeded();
        }
    }

    void release() override {
        if (railHeld) {
            railHeld = false;
            turnOffIfUnused();
        }
    }

    void otherClientAcquires() {
        otherClientHolds = true;
        turnOnIfNeeded();
    }

    void otherClientReleases() {
        otherClientHolds = false;
        turnOffIfUnused();
    }

    uint32_t getActivationCount() const override {
        return activations;
    }

    TimePoint getActivatedAt() const override {
        return activatedAt;
    }

    bool on = false;
    bool railHeld = false;
    bool otherClientHolds = false;
    uint32_t activations = 0;
    TimePoint activatedAt;
    TimePoint& now;

private:
    void turnOnIfNeeded() {
        if (!on) {
            on = true;
            activations++;
            activatedAt = now;
        }
    }

    void turnOffIfUnused() {
        if (!railHeld && !otherClientHolds) {
            on = false;
        }
    }
};

struct Fixture {
    TimePoint now {};
    std::chrono::microseconds slept {};
    std::shared_ptr<FakeRailSwitch> railSwitch = std::make_shared<FakeRailSwitch>(now);
    std::shared_ptr<SwitchedRail> rail = SwitchedRail::switchedBy(
        railSwitch,
        [this](std::chrono::microseconds duration) {
            slept += duration;
            now += duration;
        },
        [this]() { return now; });
};

}    // namespace

TEST_CASE("session powers the rail until it ends") {
    Fixture f;
    {
        auto session = f.rail->acquire();
        REQUIRE(f.railSwitch->on);
    }
    REQUIRE_FALSE(f.railSwitch->on);
}

TEST_CASE("rail stays on until the last session ends") {
    Fixture f;
    std::optional<SwitchedRail::Session> first = f.rail->acquire();
    std::optional<SwitchedRail::Session> second = f.rail->acquire();
    REQUIRE(first->getActivation() == second->getActivation());
    first.reset();
    REQUIRE(f.railSwitch->on);
    second.reset();
    REQUIRE_FALSE(f.railSwitch->on);
}

TEST_CASE("moved session releases the rail once") {
    Fixture f;
    {
        auto session = f.rail->acquire();
        auto moved = std::move(session);
        REQUIRE(f.railSwitch->on);
    }
    REQUIRE_FALSE(f.railSwitch->on);
    REQUIRE(f.railSwitch->activations == 1);
}

TEST_CASE("batch does not power the rail by itself") {
    Fixture f;
    auto batch = f.rail->batch();
    REQUIRE_FALSE(f.railSwitch->on);
}

TEST_CASE("batch keeps the rail on between sessions") {
    Fixture f;
    {
        auto batch = f.rail->batch();
        uint32_t firstActivation = f.rail->acquire().getActivation();
        REQUIRE(f.railSwitch->on);
        uint32_t secondActivation = f.rail->acquire().getActivation();
        REQUIRE(firstActivation == secondActivation);
    }
    REQUIRE_FALSE(f.railSwitch->on);
    REQUIRE(f.railSwitch->activations == 1);
}

TEST_CASE("activation changes after the rail was power-cycled") {
    Fixture f;
    uint32_t firstActivation = f.rail->acquire().getActivation();
    uint32_t secondActivation = f.rail->acquire().getActivation();
    REQUIRE(firstActivation != secondActivation);
}

TEST_CASE("waitPoweredFor waits for the full duration after power-on") {
    Fixture f;
    auto session = f.rail->acquire();
    session.waitPoweredFor(500ms);
    REQUIRE(f.slept == 500ms);
}

TEST_CASE("waitPoweredFor counts the time the rail was already on for another client") {
    Fixture f;
    f.railSwitch->otherClientAcquires();
    f.now += 200ms;
    auto session = f.rail->acquire();
    REQUIRE(session.getActivation() == 1);
    session.waitPoweredFor(500ms);
    REQUIRE(f.slept == 300ms);
}

TEST_CASE("waitPoweredFor does not wait once the duration has passed") {
    Fixture f;
    f.railSwitch->otherClientAcquires();
    f.now += 1s;
    auto session = f.rail->acquire();
    session.waitPoweredFor(500ms);
    REQUIRE(f.slept == 0ms);
}

TEST_CASE("rail stays on for another client after the last session ends") {
    Fixture f;
    f.railSwitch->otherClientAcquires();
    {
        auto session = f.rail->acquire();
    }
    REQUIRE(f.railSwitch->on);
    f.railSwitch->otherClientReleases();
    REQUIRE_FALSE(f.railSwitch->on);
}

TEST_CASE("always-on rail never waits and keeps its activation") {
    auto rail = SwitchedRail::alwaysOn();
    auto batch = rail->batch();
    uint32_t firstActivation = rail->acquire().getActivation();
    auto session = rail->acquire();
    session.waitPoweredFor(1s);
    REQUIRE(session.getActivation() == firstActivation);
    REQUIRE_FALSE(rail->isSwitched());
}

TEST_CASE("rail device configures on first power-up") {
    Fixture f;
    int configured = 0;
    RailDevice device(f.rail, 10ms, [&]() -> Result<> {
        configured++;
        return {};
    });
    REQUIRE(device.initialize());
    REQUIRE(configured == 1);
    REQUIRE(f.slept == 10ms);
}

TEST_CASE("rail device configures again only after a power cycle") {
    Fixture f;
    int configured = 0;
    RailDevice device(f.rail, 0ms, [&]() -> Result<> {
        configured++;
        return {};
    });
    {
        auto batch = f.rail->batch();
        REQUIRE(device.initialize());
        REQUIRE(device.initialize());
        REQUIRE(configured == 1);
    }
    REQUIRE(device.initialize());
    REQUIRE(configured == 2);
}

TEST_CASE("rail device on an always-on rail configures once") {
    int configured = 0;
    RailDevice device(SwitchedRail::alwaysOn(), 0ms, [&]() -> Result<> {
        configured++;
        return {};
    });
    REQUIRE(device.initialize());
    REQUIRE(device.initialize());
    REQUIRE(configured == 1);
}

TEST_CASE("rail device retries configuration after it failed") {
    Fixture f;
    int attempts = 0;
    RailDevice device(f.rail, 0ms, [&]() -> Result<> {
        if (++attempts == 1) {
            return failure("not responding");
        }
        return {};
    });
    auto batch = f.rail->batch();
    REQUIRE_FALSE(device.initialize());
    REQUIRE(device.initialize());
    REQUIRE(attempts == 2);
}
