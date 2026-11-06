#include <catch2/catch_test_macros.hpp>
#include <peripherals/environment/Smt100Protocol.hpp>
#include <peripherals/serial/MoleCricket.hpp>
#include <peripherals/serial/Sc16is752.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace cornucopia::ugly_duckling::peripherals::environment;
using namespace cornucopia::ugly_duckling::peripherals::serial;
using cornucopia::ugly_duckling::kernel::failure;
using cornucopia::ugly_duckling::kernel::Result;
using cornucopia::ugly_duckling::kernel::drivers::SwitchedRail;

// ---------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------

// Simulates the SC16IS752's register file closely enough for the driver: records every write,
// serves RX bytes queued per channel through RXLVL / RHR, and keeps the scratch-pad register.
class FakeSc16is752Bus : public ISc16is752Bus {
public:
    struct Write {
        uint8_t subaddress;
        uint8_t value;

        bool operator==(const Write&) const = default;
    };

    struct State {
        std::vector<Write> writes;
        std::map<uint8_t, std::deque<uint8_t>> rx;    // by channel
        std::map<uint8_t, std::string> tx;            // by channel
        uint8_t spr = 0;
        bool present = true;
        // Called after each TX burst, so tests can queue a response to what was sent
        std::function<void(uint8_t channel, const std::string& sent)> onTransmit;
    };

    explicit FakeSc16is752Bus(std::shared_ptr<State> state)
        : state(std::move(state)) {
    }

    Result<uint8_t> readRegister(uint8_t subaddress) override {
        if (!state->present) {
            return failure("NACK");
        }
        auto [reg, channel] = decode(subaddress);
        switch (reg) {
            case Sc16is752::REG_SPR:
                return state->spr;
            case Sc16is752::REG_RXLVL:
                return static_cast<uint8_t>(std::min<size_t>(state->rx[channel].size(), 64));
            case Sc16is752::REG_TXLVL:
                return 64;
            default:
                return 0;
        }
    }

    Result<> writeRegister(uint8_t subaddress, uint8_t value) override {
        if (!state->present) {
            return failure("NACK");
        }
        state->writes.push_back({ subaddress, value });
        auto [reg, channel] = decode(subaddress);
        if (reg == Sc16is752::REG_SPR) {
            state->spr = value;
        }
        return {};
    }

    Result<> readBurst(uint8_t subaddress, uint8_t* buffer, size_t length) override {
        auto [reg, channel] = decode(subaddress);
        REQUIRE(reg == Sc16is752::REG_RHR);
        auto& queue = state->rx[channel];
        REQUIRE(length <= queue.size());
        for (size_t i = 0; i < length; i++) {
            buffer[i] = queue.front();
            queue.pop_front();
        }
        return {};
    }

    Result<> writeBurst(uint8_t subaddress, const uint8_t* buffer, size_t length) override {
        auto [reg, channel] = decode(subaddress);
        REQUIRE(reg == Sc16is752::REG_THR);
        std::string sent(reinterpret_cast<const char*>(buffer), length);
        state->tx[channel] += sent;
        if (state->onTransmit) {
            state->onTransmit(channel, sent);
        }
        return {};
    }

    static std::pair<uint8_t, uint8_t> decode(uint8_t subaddress) {
        return { static_cast<uint8_t>((subaddress >> 3) & 0x0F), static_cast<uint8_t>((subaddress >> 1) & 0x03) };
    }

private:
    std::shared_ptr<State> state;
};

void queueRx(FakeSc16is752Bus::State& state, uint8_t channel, const std::string& data) {
    for (char c : data) {
        state.rx[channel].push_back(static_cast<uint8_t>(c));
    }
}

// Returns scripted response lines for an ISerialPort, recording what was written
class FakeSerialPort : public ISerialPort {
public:
    Result<> write(std::string_view data) override {
        written.emplace_back(data);
        return {};
    }

    Result<std::string> readUntil(char /*terminator*/, size_t /*maxLength*/, std::chrono::milliseconds /*timeout*/) override {
        if (responses.empty()) {
            return failure("no response within 1000 ms");
        }
        auto response = responses.front();
        responses.pop_front();
        return response;
    }

    Result<> flushRx() override {
        flushes++;
        return {};
    }

    std::vector<std::string> written;
    std::deque<std::string> responses;
    int flushes = 0;
};

const auto NO_DELAY = [](std::chrono::milliseconds) { };

// ---------------------------------------------------------------------------
// SC16IS752
// ---------------------------------------------------------------------------

TEST_CASE("SC16IS752 sub-address encodes register and channel") {
    CHECK(Sc16is752::subaddress(Sc16is752::REG_RHR, Sc16is752::Channel::A) == 0x00);
    CHECK(Sc16is752::subaddress(Sc16is752::REG_RHR, Sc16is752::Channel::B) == 0x02);
    CHECK(Sc16is752::subaddress(Sc16is752::REG_LCR, Sc16is752::Channel::A) == 0x18);
    CHECK(Sc16is752::subaddress(Sc16is752::REG_RXLVL, Sc16is752::Channel::B) == 0x4A);
    CHECK(Sc16is752::subaddress(Sc16is752::REG_IOCONTROL) == 0x70);
}

TEST_CASE("SC16IS752 divisor for 9600 baud from 11.0592 MHz is exactly 72") {
    auto divisor = Sc16is752::divisorFor(11'059'200, 9600);
    REQUIRE(divisor.has_value());
    CHECK(*divisor == 72);
}

TEST_CASE("SC16IS752 rejects baud rates the crystal cannot make") {
    CHECK_FALSE(Sc16is752::divisorFor(11'059'200, 0).has_value());
    CHECK_FALSE(Sc16is752::divisorFor(11'059'200, 1'000'000).has_value());
}

TEST_CASE("SC16IS752 wakes the UART, then configures 8N1 with the divisor behind the divisor latch") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    Sc16is752 chip(std::make_unique<FakeSc16is752Bus>(state), 11'059'200, NO_DELAY);
    REQUIRE(chip.configureUart(Sc16is752::Channel::B, 9600).has_value());

    using W = FakeSc16is752Bus::Write;
    auto ch = Sc16is752::Channel::B;
    std::vector<W> expected {
        // Wake from sleep left over from before a host restart
        { Sc16is752::subaddress(Sc16is752::REG_LCR, ch), 0x03 },
        { Sc16is752::subaddress(Sc16is752::REG_IER, ch), 0x00 },
        { Sc16is752::subaddress(Sc16is752::REG_LCR, ch), 0xBF },
        { Sc16is752::subaddress(Sc16is752::REG_EFR, ch), 0x10 },
        { Sc16is752::subaddress(Sc16is752::REG_LCR, ch), 0x83 },
        { Sc16is752::subaddress(Sc16is752::REG_DLL, ch), 72 },
        { Sc16is752::subaddress(Sc16is752::REG_DLH, ch), 0 },
        { Sc16is752::subaddress(Sc16is752::REG_LCR, ch), 0x03 },
        { Sc16is752::subaddress(Sc16is752::REG_MCR, ch), 0x00 },
        { Sc16is752::subaddress(Sc16is752::REG_IER, ch), 0x00 },
        { Sc16is752::subaddress(Sc16is752::REG_FCR, ch), 0x07 },
    };
    CHECK(state->writes == expected);
}

TEST_CASE("SC16IS752 presence check fails when the chip does not answer") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    state->present = false;
    Sc16is752 chip(std::make_unique<FakeSc16is752Bus>(state), 11'059'200, NO_DELAY);
    CHECK_FALSE(chip.checkPresent().has_value());
}

TEST_CASE("SC16IS752 port reads a line split across polls and keeps the rest") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    int polls = 0;
    Sc16is752 chip(std::make_unique<FakeSc16is752Bus>(state), 11'059'200, [&](std::chrono::milliseconds) {
        // The second half of the line arrives after the first wait
        if (polls++ == 0) {
            queueRx(*state, 0, ".6\r\nnext");
        }
    });
    Sc16is752Port port(chip, Sc16is752::Channel::A, std::chrono::milliseconds(5));
    queueRx(*state, 0, "24");

    auto line = port.readUntil('\n', 32, std::chrono::milliseconds(100));
    REQUIRE(line.has_value());
    CHECK(*line == "24.6\r");

    queueRx(*state, 0, "\n");
    auto next = port.readUntil('\n', 32, std::chrono::milliseconds(100));
    REQUIRE(next.has_value());
    CHECK(*next == "next");
}

TEST_CASE("SC16IS752 port times out without a response") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    int waits = 0;
    Sc16is752 chip(std::make_unique<FakeSc16is752Bus>(state), 11'059'200, [&](std::chrono::milliseconds) { waits++; });
    Sc16is752Port port(chip, Sc16is752::Channel::A, std::chrono::milliseconds(5));

    auto line = port.readUntil('\n', 32, std::chrono::milliseconds(50));
    CHECK_FALSE(line.has_value());
    CHECK(waits == 10);
}

TEST_CASE("SC16IS752 port fails on a runaway line") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    Sc16is752 chip(std::make_unique<FakeSc16is752Bus>(state), 11'059'200, NO_DELAY);
    Sc16is752Port port(chip, Sc16is752::Channel::A, std::chrono::milliseconds(5));
    queueRx(*state, 0, std::string(40, 'x'));

    CHECK_FALSE(port.readUntil('\n', 32, std::chrono::milliseconds(50)).has_value());
}

TEST_CASE("SC16IS752 port flush drains the RX FIFO") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    Sc16is752 chip(std::make_unique<FakeSc16is752Bus>(state), 11'059'200, NO_DELAY);
    Sc16is752Port port(chip, Sc16is752::Channel::B, std::chrono::milliseconds(5));
    queueRx(*state, 1, "junk");

    REQUIRE(port.flushRx().has_value());
    CHECK(state->rx[1].empty());
}

// ---------------------------------------------------------------------------
// Mole Cricket
// ---------------------------------------------------------------------------

namespace {
uint8_t lastIoState(const FakeSc16is752Bus::State& state) {
    for (auto it = state.writes.rbegin(); it != state.writes.rend(); ++it) {
        if (it->subaddress == Sc16is752::subaddress(Sc16is752::REG_IOSTATE)) {
            return it->value;
        }
    }
    FAIL("IOState never written");
    return 0;
}
}    // namespace

TEST_CASE("Mole Cricket starts with the probes off and the LED off") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    auto board = MoleCricket::create(std::make_unique<FakeSc16is752Bus>(state), SwitchedRail::alwaysOn(), NO_DELAY, "test");
    REQUIRE(board.has_value());

    // GPIO0 (probe power) low, GPIO1 (LED, active low) high
    CHECK(lastIoState(*state) == MoleCricket::GPIO_LED);
    // Levels are set before the pins become outputs
    auto ioState = std::find(state->writes.begin(), state->writes.end(),
        FakeSc16is752Bus::Write { Sc16is752::subaddress(Sc16is752::REG_IOSTATE), MoleCricket::GPIO_LED });
    auto ioDir = std::find(state->writes.begin(), state->writes.end(),
        FakeSc16is752Bus::Write { Sc16is752::subaddress(Sc16is752::REG_IODIR), MoleCricket::GPIO_PROBE_POWER | MoleCricket::GPIO_LED });
    REQUIRE(ioState != state->writes.end());
    REQUIRE(ioDir != state->writes.end());
    CHECK(ioState < ioDir);
}

TEST_CASE("Mole Cricket fails to initialize when the bridge is missing") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    state->present = false;
    CHECK_FALSE(MoleCricket::create(std::make_unique<FakeSc16is752Bus>(state), SwitchedRail::alwaysOn(), NO_DELAY, "test").has_value());
}

TEST_CASE("Mole Cricket session powers the probe only while open") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    auto board = *MoleCricket::create(std::make_unique<FakeSc16is752Bus>(state), SwitchedRail::alwaysOn(), NO_DELAY, "test");
    auto link = board->link(MoleCricket::Channel::B);
    {
        auto session = link->open();
        REQUIRE(session.has_value());
        CHECK(lastIoState(*state) == MoleCricket::GPIO_PROBE_POWER);    // probe on, LED on
        REQUIRE((*session)->write("x").has_value());
        CHECK(state->tx[1] == "x");
        queueRx(*state, 1, "junk left by the probe powering down");
    }
    CHECK(lastIoState(*state) == MoleCricket::GPIO_LED);    // probe off, LED off
    CHECK(state->rx[1].empty());
    CHECK(state->writes.back() == FakeSc16is752Bus::Write { Sc16is752::subaddress(Sc16is752::REG_IER, MoleCricket::Channel::B), Sc16is752::IER_SLEEP });
}

// ---------------------------------------------------------------------------
// SMT100 ASCII protocol
// ---------------------------------------------------------------------------

TEST_CASE("SMT100 ASCII parses value responses") {
    CHECK(*Smt100AsciiProtocol::parseValue("24.6\r") == 24.6);
    CHECK(*Smt100AsciiProtocol::parseValue("-3.25\r") == -3.25);
    CHECK(*Smt100AsciiProtocol::parseValue("0") == 0.0);
    CHECK(*Smt100AsciiProtocol::parseValue(" 12.0 \r") == 12.0);
}

TEST_CASE("SMT100 ASCII rejects malformed responses") {
    CHECK_FALSE(Smt100AsciiProtocol::parseValue("").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::parseValue("\r").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::parseValue("ERROR\r").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::parseValue("24.6x").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::parseValue("nan").has_value());
}

TEST_CASE("SMT100 ASCII validates addresses") {
    CHECK(Smt100AsciiProtocol::validateAddress("000000").has_value());
    CHECK(Smt100AsciiProtocol::validateAddress("654321").has_value());
    CHECK(Smt100AsciiProtocol::validateAddress("7").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::validateAddress("").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::validateAddress("1234567").has_value());
    CHECK_FALSE(Smt100AsciiProtocol::validateAddress("12a").has_value());
}

TEST_CASE("SMT100 ASCII reads water content and temperature") {
    auto protocol = *Smt100AsciiProtocol::create("654321", std::chrono::milliseconds(1000));
    FakeSerialPort port;
    port.responses = { "31.4\r", "18.2\r" };

    auto reading = protocol->read(port);
    REQUIRE(reading.has_value());
    CHECK(reading->moisture == 31.4);
    CHECK(reading->temperature == 18.2);
    CHECK(port.written == std::vector<std::string> { "GetWaterContent!654321\r", "GetTemperature!654321\r" });
    CHECK(port.flushes == 2);
}

TEST_CASE("SMT100 ASCII fails the reading when the probe does not answer") {
    auto protocol = *Smt100AsciiProtocol::create("000000", std::chrono::milliseconds(1000));
    FakeSerialPort port;
    port.responses = { "31.4\r" };

    auto reading = protocol->read(port);
    REQUIRE_FALSE(reading.has_value());
    CHECK(reading.error().message.starts_with("GetTemperature: "));
}

TEST_CASE("SMT100 ASCII over a Mole Cricket session") {
    auto state = std::make_shared<FakeSc16is752Bus::State>();
    state->onTransmit = [&](uint8_t channel, const std::string& sent) {
        if (sent == "GetWaterContent!000000\r") {
            queueRx(*state, channel, "27.9\r\n");
        } else if (sent == "GetTemperature!000000\r") {
            queueRx(*state, channel, "-1.5\r\n");
        }
    };
    auto board = *MoleCricket::create(std::make_unique<FakeSc16is752Bus>(state), SwitchedRail::alwaysOn(), NO_DELAY, "test");
    auto protocol = *Smt100AsciiProtocol::create("000000", std::chrono::milliseconds(1000));

    auto session = *board->link(MoleCricket::Channel::A)->open();
    auto reading = protocol->read(*session);
    REQUIRE(reading.has_value());
    CHECK(reading->moisture == 27.9);
    CHECK(reading->temperature == -1.5);
}
