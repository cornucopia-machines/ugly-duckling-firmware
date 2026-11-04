#pragma once

#include <Log.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>

#include <esp_system.h>
#include <esp_transport.h>
#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)
#include <freertos/semphr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief An esp_transport that carries a TCP connection over the modem's socket commands, so
 * esp-mqtt and the firmware download can run without lwIP (docs/Connectivity.md, "Sockets").
 *
 * Talks only to CellularModuleDriver, so it doesn't care which modem is behind it.
 *
 * Received data stays in the modem until read. The modem announces it once, and again only after
 * a read has found it drained, so reads pull it over until one comes back empty. A read also goes
 * to the modem every SAFETY_POLL_INTERVAL even without an announcement, in case one got lost.
 */
class AtSocketTransport {
public:
    AtSocketTransport()
        : handle(esp_transport_init())
        , dataSignal(xSemaphoreCreateBinary()) {
        if (handle == nullptr || dataSignal == nullptr) {
            // Only fails when out of memory, at boot
            esp_system_abort("could not create AT socket transport");
        }
        esp_transport_set_context_data(handle, this);
        esp_transport_set_func(handle, &AtSocketTransport::connect, &AtSocketTransport::read, &AtSocketTransport::write,
            &AtSocketTransport::close, &AtSocketTransport::pollRead, &AtSocketTransport::pollWrite, &AtSocketTransport::destroy);
        esp_transport_set_default_port(handle, 1883);
    }

    // The handle goes to esp-mqtt, which keeps it for as long as the client lives: forever
    AtSocketTransport(const AtSocketTransport&) = delete;
    AtSocketTransport& operator=(const AtSocketTransport&) = delete;

    /**
     * @brief Plugs in the modem once it is up; until then, connecting fails.
     *
     * Must happen before the network is reported ready, which is what lets esp-mqtt connect.
     */
    void attach(const std::shared_ptr<CellularModuleDriver>& module) {
        module->onSocketEvent([this](const SocketEvent& event) {
            onSocketEvent(event);
        });
        this->module = module;
    }

    esp_transport_handle_t getHandle() const {
        return handle;
    }

    /**
     * @brief Bytes sent and received over all connections since the last call, for telemetry.
     *
     * TCP and IP headers, ACKs and retransmissions aren't included, so the data the operator
     * counts is somewhat more.
     */
    std::pair<size_t, size_t> takeTrafficCounts() {
        return { trafficSent.exchange(0), trafficReceived.exchange(0) };
    }

    /**
     * @brief How many times the network was asked to release the RRC connection (see
     * releaseIfDone()) since the last call.
     */
    uint32_t takeReleaseRequests() {
        return releaseRequests.exchange(0);
    }

private:
    static AtSocketTransport& from(esp_transport_handle_t transport) {
        return *static_cast<AtSocketTransport*>(esp_transport_get_context_data(transport));
    }

    static int connect(esp_transport_handle_t transport, const char* host, int port, int /*timeoutMs*/) {
        // The modem has its own timeout for connecting, a lot longer than esp-mqtt's
        return from(transport).connect(host, port);
    }

    static int read(esp_transport_handle_t transport, char* buffer, int length, int timeoutMs) {
        return from(transport).read(reinterpret_cast<uint8_t*>(buffer), static_cast<size_t>(length), toTimeout(timeoutMs));
    }

    static int write(esp_transport_handle_t transport, const char* buffer, int length, int /*timeoutMs*/) {
        return from(transport).write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length));
    }

    static int close(esp_transport_handle_t transport) {
        return from(transport).close();
    }

    static int pollRead(esp_transport_handle_t transport, int timeoutMs) {
        return from(transport).pollRead(toTimeout(timeoutMs));
    }

    // esp_transport's -1 means waiting for good
    static milliseconds toTimeout(int timeoutMs) {
        return timeoutMs < 0 ? milliseconds(24h) : milliseconds(timeoutMs);
    }

    static int pollWrite(esp_transport_handle_t /*transport*/, int /*timeoutMs*/) {
        // The modem takes data whenever it answers commands
        return 1;
    }

    static int destroy(esp_transport_handle_t /*transport*/) {
        return 0;
    }

    int connect(const char* host, int port) {
        auto module = this->module;
        if (module == nullptr) {
            LOGTW(CELLULAR, "Cannot connect to %s:%d, the modem is not up", host, port);
            return -1;
        }
        bufferStart = 0;
        bufferEnd = 0;
        moreWaiting = false;
        closedByPeer = false;
        xSemaphoreTake(dataSignal, 0);
        bytesSent = 0;
        bytesReceived = 0;
        bytesFoundBySafetyPoll = 0;
        announcementsAtConnect = announcements.load();
        uartErrorsAtConnect = module->getUartErrorCount();
        connectedAt = steady_clock::now();
        if (!module->openSocket(host, port)) {
            return -1;
        }
        // Whatever arrived while connecting was for a connection before this one
        closedByPeer = false;
        connected = true;
        lastReceive = steady_clock::now();
        lastActivity = lastReceive;
        releaseDue = false;
        return 0;
    }

    int read(uint8_t* buffer, size_t length, milliseconds timeout) {
        if (bufferStart == bufferEnd) {
            int ready = pollRead(timeout);
            if (ready <= 0) {
                return ready < 0 ? ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN : ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
            }
        }
        size_t count = std::min(length, bufferEnd - bufferStart);
        std::memcpy(buffer, receiveBuffer.data() + bufferStart, count);
        bufferStart += count;
        return static_cast<int>(count);
    }

    /**
     * @return 1 when there is data to read, 0 on timeout, -1 when the connection is gone
     */
    int pollRead(milliseconds timeout) {
        if (bufferStart < bufferEnd) {
            return 1;
        }
        if (!connected) {
            return -1;
        }
        // A read that finds nothing doesn't end the wait: the caller gets 0 only once its whole
        // timeout has passed, or it would give up on an answer still on its way
        auto deadline = steady_clock::now() + timeout;
        while (true) {
            auto left = duration_cast<milliseconds>(deadline - steady_clock::now());
            auto reason = shouldReceive(std::max(left, 0ms));
            if (reason == ReceiveReason::None) {
                return 0;
            }
            if (!receive()) {
                return -1;
            }
            if (bufferStart < bufferEnd) {
                if (reason == ReceiveReason::SafetyPoll) {
                    // Tells how often announcements get lost, which bounds how late such data is
                    LOGTD(CELLULAR, "Found %zu bytes the modem didn't announce", bufferEnd - bufferStart);
                    bytesFoundBySafetyPoll += bufferEnd - bufferStart;
                }
                return 1;
            }
            if (closedByPeer) {
                // Whatever the peer sent before closing has been read by now
                return -1;
            }
            if (steady_clock::now() >= deadline) {
                return 0;
            }
        }
    }

    enum class ReceiveReason : uint8_t {
        None,
        // The modem said there's data, or that the connection was closed
        Announced,
        // The last read wasn't empty yet
        Draining,
        // It's been SAFETY_POLL_INTERVAL since the last read
        SafetyPoll,
    };

    /**
     * @brief Whether to ask the modem for data: it said it has some, a read is draining what it
     * has, or it's been a while. Waits up to the timeout for one of these.
     */
    ReceiveReason shouldReceive(milliseconds timeout) {
        auto deadline = steady_clock::now() + timeout;
        while (true) {
            if (moreWaiting) {
                return ReceiveReason::Draining;
            }
            if (closedByPeer || uxSemaphoreGetCount(dataSignal) > 0) {
                return ReceiveReason::Announced;
            }
            auto now = steady_clock::now();
            if (releaseDue && now - lastActivity >= RELEASE_AFTER) {
                releaseIfDone();
            }
            if (now - lastReceive >= SAFETY_POLL_INTERVAL) {
                return ReceiveReason::SafetyPoll;
            }
            if (now >= deadline) {
                return ReceiveReason::None;
            }
            auto wakeAt = std::min(deadline, lastReceive + SAFETY_POLL_INTERVAL);
            if (releaseDue) {
                wakeAt = std::min(wakeAt, lastActivity + RELEASE_AFTER);
            }
            auto wait = std::max<TickType_t>(1, pdMS_TO_TICKS(duration_cast<milliseconds>(wakeAt - now).count()));
            if (xSemaphoreTake(dataSignal, wait) == pdTRUE) {
                // Left for receive() to take, which it does before reading
                xSemaphoreGive(dataSignal);
                return ReceiveReason::Announced;
            }
        }
    }

    /**
     * @brief Once an exchange is over, asks the network to release the RRC connection, instead
     * of keeping the radio connected until the network's inactivity timer runs out, which is
     * where most of the modem's energy goes.
     *
     * Over? When the last thing was data received, and nothing more came for RELEASE_AFTER:
     * whatever we sent got its answer (a PUBACK, a PINGRESP), and the modem has sent its TCP
     * ACKs by then. After a send, we're waiting for an answer, so it's left to the network's
     * timer. Guessing wrong only costs a new connection: the network pages us for what it still
     * has, and our next send connects again.
     */
    void releaseIfDone() {
        releaseDue = false;
        auto module = this->module;
        if (module != nullptr && module->releaseRrc()) {
            releaseRequests++;
        }
    }

    /**
     * @brief Pulls the next chunk of received data from the modem into receiveBuffer.
     *
     * @return false if the modem didn't answer
     */
    bool receive() {
        auto module = this->module;
        // Take the signal before reading: an announcement arriving after this belongs to newer data
        xSemaphoreTake(dataSignal, 0);
        lastReceive = steady_clock::now();
        auto result = module->receive(receiveBuffer.data(), receiveBuffer.size());
        if (!result) {
            return false;
        }
        bufferStart = 0;
        bufferEnd = result->length;
        if (result->length > 0) {
            lastActivity = steady_clock::now();
            releaseDue = true;
        }
        if (result->closed) {
            closedByPeer = true;
        }
        // Keep reading until a read comes back empty: the modem only announces new data once a
        // read has found it drained
        moreWaiting = result->length > 0;
        bytesReceived += result->length;
        trafficReceived += result->length;
        return true;
    }

    int write(const uint8_t* data, size_t length) {
        auto module = this->module;
        if (!connected || module == nullptr) {
            return -1;
        }
        size_t written = 0;
        while (written < length) {
            size_t chunk = std::min(length - written, module->getMaxSendSize());
            if (!module->send(data + written, chunk)) {
                return -1;
            }
            written += chunk;
            bytesSent += chunk;
            trafficSent += chunk;
        }
        // An answer is likely to follow
        lastActivity = steady_clock::now();
        releaseDue = false;
        return static_cast<int>(written);
    }

    int close() {
        auto module = this->module;
        if (!connected || module == nullptr) {
            return 0;
        }
        connected = false;
        module->closeSocket();
        LOGTI(CELLULAR, "Connection closed after %lld s, %zu bytes sent, %zu bytes received "
                        "(%zu found by the safety poll), %" PRIu32 " data announcements, %" PRIu32 " UART errors",
            static_cast<long long>(duration_cast<seconds>(steady_clock::now() - connectedAt).count()),
            bytesSent, bytesReceived, bytesFoundBySafetyPoll,
            announcements.load() - announcementsAtConnect,
            module->getUartErrorCount() - uartErrorsAtConnect);
        return 0;
    }

    // Runs on the UART's reader task, so only flags things for the next read
    void onSocketEvent(const SocketEvent& event) {
        switch (event.type) {
            case SocketEventType::DataAvailable:
                announcements++;
                break;
            case SocketEventType::Closed:
                if (connected) {
                    LOGTI(CELLULAR, "Connection closed by the peer or the network");
                }
                closedByPeer = true;
                break;
        }
        xSemaphoreGive(dataSignal);
    }

    // How often to look for received data without the modem having said there is any, in case
    // an announcement got lost: one that came just as the modem's UART powered down, say. A read
    // costs a UART wake-up, not a radio one
    static constexpr milliseconds SAFETY_POLL_INTERVAL = 30s;
    // How long after the last data received the exchange counts as over (see releaseIfDone()).
    // Covers a round trip even on NB-IoT
    static constexpr milliseconds RELEASE_AFTER = 5s;

    esp_transport_handle_t handle;
    const SemaphoreHandle_t dataSignal;
    std::shared_ptr<CellularModuleDriver> module;

    // Everything below is only touched from the task esp-mqtt runs the transport on, except for
    // closedByPeer and the counters, which the socket event handler and the telemetry touch
    bool connected = false;
    std::atomic<bool> closedByPeer { false };
    bool moreWaiting = false;
    steady_clock::time_point lastReceive;
    // When data was last sent, or received from the modem
    steady_clock::time_point lastActivity;
    // Whether the last thing on the connection was data received, and the RRC connection hasn't
    // been released since (see releaseIfDone())
    bool releaseDue = false;

    std::atomic<uint32_t> announcements { 0 };
    uint32_t announcementsAtConnect = 0;
    uint32_t uartErrorsAtConnect = 0;
    // Taken by the telemetry, on another task
    std::atomic<uint32_t> releaseRequests { 0 };

    // The most the modem returns in one read
    std::array<uint8_t, 2048> receiveBuffer {};
    size_t bufferStart = 0;
    size_t bufferEnd = 0;

    steady_clock::time_point connectedAt;
    size_t bytesSent = 0;
    size_t bytesReceived = 0;
    size_t bytesFoundBySafetyPoll = 0;

    // Across connections, read from the telemetry task
    std::atomic<size_t> trafficSent { 0 };
    std::atomic<size_t> trafficReceived { 0 };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
