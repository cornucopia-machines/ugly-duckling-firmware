#pragma once

/*
 * BitbangI2CBus — bitbang I2C master state machine for the ESP32.
 *
 * The file has two sections:
 *
 *   1. Pure C++ (always compiled): IBitbangPin interface and BitbangI2CBus.
 *      No ESP-IDF headers — this section compiles in native unit tests.
 *      Transport primitives (delay, critical sections) are virtual no-ops;
 *      tests use them as-is and production code overrides them.
 *
 *   2. ESP-IDF adapters (#ifdef ESP_PLATFORM): EspBitbangPin wraps
 *      InternalPin with ISR-safe setLevel/getLevel; EspBitbangI2CBus adds
 *      5 µs half-bit delays and per-byte portENTER_CRITICAL sections.
 *      SpadefootToadSensorWithBitbangI2C uses these two classes directly.
 */

#include <cstdint>
#include <stdexcept>
#include <string>

namespace cornucopia::ugly_duckling::peripherals::environment {

// ---------------------------------------------------------------------------
// Minimal GPIO interface for SDA / SCL lines.
// ---------------------------------------------------------------------------

class IBitbangPin {
public:
    virtual const std::string& getName() const = 0;
    virtual void setLevel(int level) = 0;
    virtual int getLevel() = 0;
    virtual ~IBitbangPin() = default;
};

// ---------------------------------------------------------------------------
// Bitbang I2C master — 100 kHz, open-drain, clock stretching.
//
// Subclass and override halfBitDelay / enterCritical / exitCritical with real
// implementations for production use. The defaults are no-ops, which is
// correct for unit tests.
// ---------------------------------------------------------------------------

class BitbangI2CBus {
public:
    virtual ~BitbangI2CBus() = default;

    // stretchTimeout: iterations to wait for SCL to go high after releasing it.
    // Each iteration corresponds to one halfBitDelay() call.
    // Default 10 000 → ≈10 ms at 100 kHz. Lower values are used in tests.
    explicit BitbangI2CBus(IBitbangPin& sda, IBitbangPin& scl, int stretchTimeout = 10000)
        : sda(sda)
        , scl(scl)
        , stretchTimeout(stretchTimeout) {
    }

    // I2C START: SDA goes low while SCL is high.
    void start() {
        sda.setLevel(1);
        scl.setLevel(1);
        halfBitDelay();
        sda.setLevel(0);
        halfBitDelay();
        scl.setLevel(0);
        halfBitDelay();
    }

    // I2C REPEATED START: release SDA then SCL, then pull SDA low (SCL high).
    void repeatedStart() {
        sda.setLevel(1);
        halfBitDelay();
        scl.setLevel(1);
        halfBitDelay();
        sda.setLevel(0);
        halfBitDelay();
        scl.setLevel(0);
        halfBitDelay();
    }

    // I2C STOP: pull SDA low, raise SCL, then raise SDA.
    void stop() {
        sda.setLevel(0);
        halfBitDelay();
        scl.setLevel(1);
        halfBitDelay();
        sda.setLevel(1);
        halfBitDelay();
    }

    // Write one byte (MSB first) and read the ACK/NACK bit.
    // Critical sections are per-byte (8 data bits + ACK = ≤90 µs at 100 kHz).
    // Returns true on ACK (slave drove SDA low), false on NACK.
    // Throws std::runtime_error on clock-stretch timeout.
    bool writeByteGetAck(uint8_t byte) {
        enterCritical();

        for (int i = 7; i >= 0; i--) {
            sda.setLevel((byte >> i) & 1);
            halfBitDelay();
            if (!sclRelease()) {
                exitCritical();
                throw std::runtime_error("BitbangI2C: clock stretch timeout");
            }
            halfBitDelay();
            scl.setLevel(0);
        }

        // Release SDA for ACK
        sda.setLevel(1);
        halfBitDelay();
        if (!sclRelease()) {
            exitCritical();
            throw std::runtime_error("BitbangI2C: clock stretch timeout on ACK");
        }
        halfBitDelay();
        bool ack = (sda.getLevel() == 0);    // ACK = slave drives SDA low
        scl.setLevel(0);

        exitCritical();
        return ack;
    }

    // Read one byte (MSB first) and send ACK (nack=false) or NACK (nack=true).
    // Throws std::runtime_error on clock-stretch timeout.
    uint8_t readByteAndAck(bool nack) {
        uint8_t byte = 0;
        enterCritical();

        for (int i = 7; i >= 0; i--) {
            sda.setLevel(1);    // release SDA so slave can drive it
            halfBitDelay();
            if (!sclRelease()) {
                exitCritical();
                throw std::runtime_error("BitbangI2C: clock stretch timeout");
            }
            halfBitDelay();
            if (sda.getLevel() != 0) {
                byte |= (1U << i);
            }
            scl.setLevel(0);
        }

        // Send ACK or NACK
        sda.setLevel(nack ? 1 : 0);
        halfBitDelay();
        if (!sclRelease()) {
            exitCritical();
            throw std::runtime_error("BitbangI2C: clock stretch timeout on ACK/NACK");
        }
        halfBitDelay();
        scl.setLevel(0);
        sda.setLevel(1);    // release SDA

        exitCritical();
        return byte;
    }

protected:
    // Override with esp_rom_delay_us(5) in production.
    virtual void halfBitDelay() {
    }

    // Override with portENTER_CRITICAL / portEXIT_CRITICAL in production.
    virtual void enterCritical() {
    }
    virtual void exitCritical() {
    }

private:
    IBitbangPin& sda;
    IBitbangPin& scl;
    const int stretchTimeout;

    // Release SCL and wait (up to stretchTimeout iterations) for the slave to release it.
    // Returns false if the slave held SCL low past the timeout.
    bool sclRelease() {
        scl.setLevel(1);
        for (int i = 0; i < stretchTimeout; i++) {
            if (scl.getLevel() != 0) {
                return true;
            }
            halfBitDelay();
        }
        return false;    // clock stretch timeout
    }
};

}    // namespace cornucopia::ugly_duckling::peripherals::environment

// ---------------------------------------------------------------------------
// ESP-IDF adapters — only compiled when building for ESP32.
// These are the two classes SpadefootToadSensorWithBitbangI2C uses directly.
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

#include <Pin.hpp>

namespace cornucopia::ugly_duckling::peripherals::environment {

using cornucopia::ugly_duckling::kernel::InternalPinPtr;

// IBitbangPin adapter: wraps InternalPin, uses ISR-safe calls for setLevel/getLevel.
class EspBitbangPin : public IBitbangPin {
public:
    explicit EspBitbangPin(const InternalPinPtr& pin)
        : pin(pin) {
    }

    const std::string& getName() const override {
        return pin->getName();
    }

    IRAM_ATTR void setLevel(int level) override {
        pin->digitalWriteFromISR(static_cast<uint8_t>(level));
    }

    IRAM_ATTR int getLevel() override {
        return pin->digitalReadFromISR();
    }

private:
    InternalPinPtr pin;
};

// BitbangI2CBus subclass: adds real 5 µs half-bit delays and per-byte critical sections.
class EspBitbangI2CBus : public BitbangI2CBus {
public:
    using BitbangI2CBus::BitbangI2CBus;

protected:
    IRAM_ATTR void halfBitDelay() override {
        esp_rom_delay_us(5);    // 100 kHz: 10 µs period → 5 µs half-bit
    }

    IRAM_ATTR void enterCritical() override {
        portENTER_CRITICAL(&critMux);
    }

    IRAM_ATTR void exitCritical() override {
        portEXIT_CRITICAL(&critMux);
    }

private:
    portMUX_TYPE critMux = portMUX_INITIALIZER_UNLOCKED;
};

}    // namespace cornucopia::ugly_duckling::peripherals::environment

#endif    // ESP_PLATFORM
