#pragma once

#include <Log.hpp>
#include <drivers/cellular/AtSocket.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>

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
#include <optional>
#include <stdexcept>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief An esp_transport that carries a TCP connection over the cellular module's socket
 * commands, so esp-mqtt can run over a modem that has no PPP (docs/specs/NB-IoT.md).
 *
 * Modelled on tcp_transport_at.cpp from esp_modem's modem_tcp_client example, but talks only to
 * CellularModuleDriver, so it doesn't care which module is behind it.
 *
 * The module keeps received data in its own buffer (buffer access mode) and says so with a URC;
 * reads pull it over in chunks of up to getMaxReceiveSize() bytes, until one comes back empty
 * (see receive()). While data is flowing, the module is also polled every ACTIVE_POLL_INTERVAL, since its URCs come too late then (see
 * shouldReceive()); otherwise, because a URC can be lost, a read also goes to the module every
 * SAFETY_POLL_INTERVAL even without one.
 */
class AtSocketTransport {
public:
    AtSocketTransport()
        : handle(esp_transport_init())
        , dataSignal(xSemaphoreCreateBinary()) {
        if (handle == nullptr || dataSignal == nullptr) {
            throw std::runtime_error("could not create AT socket transport");
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
     * @brief Plugs in the module once it is up; until then, connecting fails.
     *
     * Must happen before the network is reported ready, which is what lets esp-mqtt connect.
     */
    void attach(const std::shared_ptr<CellularModuleDriver>& module) {
        module->onSocketEvent([this](SocketEventType type) {
            onSocketEvent(type);
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
     * @brief Makes the next poll ask the module for data, for when it may have announced some in
     * a URC that didn't arrive intact.
     */
    void checkForData() {
        unrecognizedLines++;
        xSemaphoreGive(dataSignal);
    }

private:
    static AtSocketTransport& from(esp_transport_handle_t transport) {
        return *static_cast<AtSocketTransport*>(esp_transport_get_context_data(transport));
    }

    static int connect(esp_transport_handle_t transport, const char* host, int port, int /*timeoutMs*/) {
        // The module has its own timeout for connecting, a lot longer than esp-mqtt's
        return from(transport).connect(host, port);
    }

    static int read(esp_transport_handle_t transport, char* buffer, int length, int timeoutMs) {
        return from(transport).read(reinterpret_cast<uint8_t*>(buffer), static_cast<size_t>(length), milliseconds(timeoutMs));
    }

    static int write(esp_transport_handle_t transport, const char* buffer, int length, int /*timeoutMs*/) {
        return from(transport).write(reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(length));
    }

    static int close(esp_transport_handle_t transport) {
        return from(transport).close();
    }

    static int pollRead(esp_transport_handle_t transport, int timeoutMs) {
        return from(transport).pollRead(milliseconds(timeoutMs));
    }

    static int pollWrite(esp_transport_handle_t /*transport*/, int /*timeoutMs*/) {
        // The module takes data whenever it answers commands
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
        announcementsAtConnect = announcements.load();
        announcementsAtLastReceive = announcementsAtConnect;
        unrecognizedLinesAtLastReceive = unrecognizedLines.load();
        receiveBufferFilling = false;
        connectedAt = steady_clock::now();
        if (!module->openSocket(host, port)) {
            return -1;
        }
        connected = true;
        lastReceive = steady_clock::now();
        lastActivity = lastReceive;
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
        // A poll that finds nothing doesn't end the wait: the caller gets 0 only once its whole
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
                    // Tells how often URCs get lost altogether, which bounds how late such data is
                    LOGTD(CELLULAR, "Found %zu bytes the modem didn't announce", bufferEnd - bufferStart);
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
        // The module said there's data, or may have (see checkForData()), or the last read
        // wasn't empty yet
        Announced,
        // Data is flowing, and it's been ACTIVE_POLL_INTERVAL since the last read
        ActivePoll,
        // It's been SAFETY_POLL_INTERVAL since the last read
        SafetyPoll,
    };

    /**
     * @brief Whether to ask the module for data: it told us it has some, data is flowing, or
     * it's been a while. Waits up to the timeout for one of these.
     *
     * While data is flowing, the module's announcements can't be relied on: during downloads it
     * stayed silent for seconds while its buffer filled, and only spoke up once the buffer was
     * full, by when it was dropping data (docs/specs/NB-IoT.md, "`QISEND` / `QIRD` size limits").
     * Going by the BG96's documented behavior, it announces again only once a read has found
     * the buffer empty. So for a while after sending or receiving anything, it's polled.
     */
    ReceiveReason shouldReceive(milliseconds timeout) {
        if (moreWaiting || closedByPeer) {
            return ReceiveReason::Announced;
        }
        auto now = steady_clock::now();
        auto sinceLastReceive = duration_cast<milliseconds>(now - lastReceive);
        if (sinceLastReceive >= SAFETY_POLL_INTERVAL) {
            return ReceiveReason::SafetyPoll;
        }
        bool flowing = now - lastActivity < ACTIVE_WINDOW;
        if (flowing && sinceLastReceive >= ACTIVE_POLL_INTERVAL) {
            return ReceiveReason::ActivePoll;
        }
        auto untilPoll = (flowing ? ACTIVE_POLL_INTERVAL : SAFETY_POLL_INTERVAL) - sinceLastReceive;
        auto wait = std::min(timeout, untilPoll);
        if (xSemaphoreTake(dataSignal, pdMS_TO_TICKS(wait.count())) == pdTRUE) {
            return ReceiveReason::Announced;
        }
        if (wait < timeout) {
            return flowing ? ReceiveReason::ActivePoll : ReceiveReason::SafetyPoll;
        }
        return ReceiveReason::None;
    }

    /**
     * @brief Pulls the next chunk of received data from the module into receiveBuffer.
     *
     * @return false if the module didn't answer
     */
    bool receive() {
        auto module = this->module;
        // Take the signal before reading: a URC arriving after this belongs to newer data
        xSemaphoreTake(dataSignal, 0);
        auto sinceLastReceive = steady_clock::now() - lastReceive;
        lastReceive = steady_clock::now();
        auto announced = announcements.load();
        auto unrecognized = unrecognizedLines.load();
        auto result = module->receive(receiveBuffer.data(), receiveBuffer.size());
        if (!result) {
            return false;
        }
        logIfBufferFilling(result->remaining, sinceLastReceive, announced, unrecognized);
        bufferStart = 0;
        bufferEnd = result->length;
        if (result->length > 0) {
            lastActivity = steady_clock::now();
        }
        // Keep reading until a read comes back empty, not just until the module says nothing is
        // left: like the BG96, it seems to announce new data only once a read has found its
        // buffer empty, so stopping at the last byte would leave the next data unannounced
        moreWaiting = result->more || result->length > 0;
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
        return static_cast<int>(written);
    }

    int close() {
        auto module = this->module;
        if (!connected || module == nullptr) {
            return 0;
        }
        connected = false;
        module->closeSocket();
        LOGTI(CELLULAR, "Connection closed after %lld s, %zu bytes sent, %zu bytes received, %" PRIu32 " data announcements",
            static_cast<long long>(duration_cast<seconds>(steady_clock::now() - connectedAt).count()),
            bytesSent, bytesReceived, announcements.load() - announcementsAtConnect);
        return 0;
    }

    /**
     * @brief Logs when the module's receive buffer fills past the half mark: once it's full, the
     * module drops what arrives (docs/specs/NB-IoT.md, "`QISEND` / `QIRD` size limits"). Says
     * whether the module announced the data since the last read, which tells a lost URC apart
     * from data that came in faster than it was read. Once per crossing, not on every read.
     */
    void logIfBufferFilling(std::optional<size_t> remaining, steady_clock::duration sinceLastReceive, uint32_t announced, uint32_t unrecognized) {
        bool filling = remaining && *remaining >= RECEIVE_BUFFER_WARNING;
        if (filling && !receiveBufferFilling) {
            if (announced != announcementsAtLastReceive) {
                LOGTD(CELLULAR, "%zu bytes waiting in the modem; last read %lld ms ago, data announced %lld ms ago",
                    *remaining,
                    static_cast<long long>(duration_cast<milliseconds>(sinceLastReceive).count()),
                    static_cast<long long>(duration_cast<milliseconds>(steady_clock::now() - lastAnnouncedAt.load()).count()));
            } else {
                LOGTD(CELLULAR, "%zu bytes waiting in the modem; last read %lld ms ago, not announced since%s",
                    *remaining,
                    static_cast<long long>(duration_cast<milliseconds>(sinceLastReceive).count()),
                    unrecognized != unrecognizedLinesAtLastReceive ? " (but an unrecognized line came)" : "");
            }
        }
        receiveBufferFilling = filling;
        announcementsAtLastReceive = announced;
        unrecognizedLinesAtLastReceive = unrecognized;
    }

    // Runs on the UART's receive task, so only flags things for the next read
    void onSocketEvent(SocketEventType type) {
        switch (type) {
            case SocketEventType::DataAvailable:
                announcements++;
                lastAnnouncedAt = steady_clock::now();
                break;
            case SocketEventType::BufferFull:
                LOGTW(CELLULAR, "Modem receive buffer full");
                announcements++;
                lastAnnouncedAt = steady_clock::now();
                break;
            case SocketEventType::Closed:
                LOGTI(CELLULAR, "Connection closed by the peer or the network");
                closedByPeer = true;
                break;
        }
        xSemaphoreGive(dataSignal);
    }

    // How often to look for received data without the module having said there is any. Every read
    // wakes the module, and it stays awake for 10 s after UART activity (AT+QCFG="slplocktimes"),
    // so polling every 10 s would keep it awake for good. A URC that wakes the ESP32 mostly
    // arrives, just without its first bytes, so this only catches the rare one lost entirely
    static constexpr milliseconds SAFETY_POLL_INTERVAL = 2min;
    // How often to look for received data while it's flowing, and for how long after the last
    // data sent or received. At about 2 KB/s, 250 ms is around 500 bytes, well inside the
    // module's 2168-byte buffer; the window covers an NB-IoT round trip
    static constexpr milliseconds ACTIVE_POLL_INTERVAL = 250ms;
    static constexpr milliseconds ACTIVE_WINDOW = 5s;
    // Half the module's receive buffer (2168 bytes on the BC660K)
    static constexpr size_t RECEIVE_BUFFER_WARNING = 1024;

    esp_transport_handle_t handle;
    const SemaphoreHandle_t dataSignal;
    std::shared_ptr<CellularModuleDriver> module;

    // Everything below is only touched from the task esp-mqtt runs the transport on, except for
    // closedByPeer, which the URC handler sets
    bool connected = false;
    std::atomic<bool> closedByPeer { false };
    bool moreWaiting = false;
    steady_clock::time_point lastReceive;
    // When data was last sent, or received from the module
    steady_clock::time_point lastActivity;

    // Data announcements (URCs) and unrecognized lines from the module, counted by the URC
    // handler, for telling a lost URC apart from data that arrived faster than it was read
    std::atomic<uint32_t> announcements { 0 };
    std::atomic<steady_clock::time_point> lastAnnouncedAt;
    std::atomic<uint32_t> unrecognizedLines { 0 };
    uint32_t announcementsAtConnect = 0;
    uint32_t announcementsAtLastReceive = 0;
    uint32_t unrecognizedLinesAtLastReceive = 0;
    bool receiveBufferFilling = false;

    std::array<uint8_t, 512> receiveBuffer {};
    size_t bufferStart = 0;
    size_t bufferEnd = 0;

    steady_clock::time_point connectedAt;
    size_t bytesSent = 0;
    size_t bytesReceived = 0;

    // Across connections, read from the telemetry task
    std::atomic<size_t> trafficSent { 0 };
    std::atomic<size_t> trafficReceived { 0 };
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
