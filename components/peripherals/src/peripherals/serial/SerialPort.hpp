#pragma once

/*
 * Transport-neutral serial interfaces for sensors on a UART.
 *
 * A serial sensor driver (e.g. SMT100) talks to an ISerialPort, and gets one by opening a session on
 * an ISerialLink. The link owns everything about getting a working line to the sensor: exclusive
 * access to a shared bridge or bus, switching the sensor's supply on, and switching it off again when
 * the session ends. This keeps the protocol code independent of whether the UART is an SC16IS752
 * channel on a Mole Cricket board or the ESP's own UART behind an RS485 transceiver (Rs485Bus).
 *
 * Pure C++: no ESP-IDF headers, so protocol code built on this compiles in native unit tests.
 */

#include <Result.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

namespace cornucopia::ugly_duckling::peripherals::serial {

using cornucopia::ugly_duckling::kernel::Result;

struct ISerialPort {
    virtual ~ISerialPort() = default;

    virtual Result<> write(std::string_view data) = 0;

    // Reads exactly `length` bytes, for binary protocols whose frames carry their own length (e.g.
    // Modbus RTU). Fails if they don't all arrive within `timeout`.
    virtual Result<std::string> read(size_t length, std::chrono::milliseconds timeout) = 0;

    // Reads until `terminator` arrives and returns the bytes before it (the terminator is dropped).
    // Fails if no terminator arrives within `timeout`, or after `maxLength` bytes.
    virtual Result<std::string> readUntil(char terminator, size_t maxLength, std::chrono::milliseconds timeout) = 0;

    // Discards anything already received.
    virtual Result<> flushRx() = 0;
};

// An open line to a powered sensor. Destroying the session switches the sensor off and releases the
// link for other users.
struct ISerialSession : ISerialPort {
    // Waits until the sensor has been powered for at least `duration`, i.e. for its start-up time.
    // Time it was already powered before the session opened (e.g. on a rail kept on by a telemetry
    // batch) counts towards it.
    virtual void waitPoweredFor(std::chrono::milliseconds duration) = 0;
};

struct ISerialLink {
    virtual ~ISerialLink() = default;

    // Powers the sensor and returns the open line. Blocks while another session holds a link that
    // shares the same hardware (e.g. the other channel of the same bridge, whose probe shares the
    // supply switch). The sensor's own startup time is the caller's business: see
    // ISerialSession::waitPoweredFor().
    virtual Result<std::unique_ptr<ISerialSession>> open() = 0;

    virtual std::string describe() const = 0;
};

// A multi-drop bus (RS485) that sensors share, each reached at its own baud rate.
struct ISerialBus {
    virtual ~ISerialBus() = default;

    virtual std::shared_ptr<ISerialLink> link(uint32_t baud) = 0;
};

}    // namespace cornucopia::ugly_duckling::peripherals::serial
