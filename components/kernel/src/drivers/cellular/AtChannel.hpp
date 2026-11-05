#pragma once

#include <EspError.hpp>
#include <Log.hpp>
#include <Result.hpp>
#include <Task.hpp>
#include <drivers/cellular/AtResponse.hpp>
#include <drivers/cellular/CellularModemPins.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>

#include <driver/gpio.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>    // NOLINT(misc-header-include-cycle)
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief AT commands and URCs over a UART with hardware flow control.
 *
 * A reader task splits what the modem sends into lines. While a command is in flight, its
 * response lines go to the command, and everything the isUrc predicate recognizes goes to the
 * URC handler, as does everything that arrives between commands.
 *
 * Commands aren't serialized here: the caller sends one at a time.
 */
class AtChannel {
public:
    // Whether a line is a URC rather than part of the response to the command in flight
    using UrcPredicate = std::function<bool(std::string_view line, std::string_view command)>;
    // Called on the reader task whenever data arrives from the modem
    using ActivityHandler = std::function<void()>;

    static Result<std::unique_ptr<AtChannel>> create(uart_port_t port, const CellularModemPins& pins, int baudRate, UrcPredicate isUrc) {
        uart_config_t config {};
        config.baud_rate = baudRate;
        config.data_bits = UART_DATA_8_BITS;
        config.parity = UART_PARITY_DISABLE;
        config.stop_bits = UART_STOP_BITS_1;
        config.flow_ctrl = UART_HW_FLOWCTRL_CTS_RTS;
        // RTS goes up when the hardware FIFO (128 bytes) fills this far: the interrupt empties it
        // into the driver's buffer long before that, even while flash writes stall everything not
        // in IRAM (CONFIG_UART_ISR_IN_IRAM)
        config.rx_flow_ctrl_thresh = 100;
        // Not the default PLL clock: before light sleep, IDF suspends every enabled UART and waits
        // for it to sync its registers, which needs the UART's clock running. On the PLL that
        // wait never ended, and the device froze the first time it went to sleep. XTAL is also
        // what IDF recommends for peripherals while the CPU frequency scales
        config.source_clk = UART_SCLK_XTAL;

        QueueHandle_t events = nullptr;
        // No TX buffer: commands go straight into the hardware FIFO (see write()), so that nothing
        // is left queued behind a modem that stopped taking data
        ESP_TRY(uart_driver_install(port, RX_BUFFER_SIZE, 0, EVENT_QUEUE_SIZE, &events, 0));
        ESP_TRY(uart_param_config(port, &config));
        ESP_TRY(uart_set_pin(port, pins.tx->getGpio(), pins.rx->getGpio(), pins.rts->getGpio(), pins.cts->getGpio()));
        // Serial Modem asks the host to pull its CTS up: while the modem's UART is off, its RTS
        // isn't driven, and we must not send into it
        ESP_TRY(gpio_pullup_en(pins.cts->getGpio()));
        return std::unique_ptr<AtChannel>(new AtChannel(port, events, std::move(isUrc)));
    }

    AtChannel(const AtChannel&) = delete;
    AtChannel& operator=(const AtChannel&) = delete;

    void onUrc(UrcHandler handler) {
        std::scoped_lock lock(mutex);
        urcHandler = std::move(handler);
    }

    void onActivity(ActivityHandler handler) {
        std::scoped_lock lock(mutex);
        activityHandler = std::move(handler);
    }

    /**
     * @brief Sends a command and waits for its response, up to the timeout.
     *
     * A command that times out still gets its answer from the modem eventually; the next
     * command first waits for that, so that the late result isn't taken for its own.
     */
    AtResponse command(const std::string& command, milliseconds timeout) {
        if (partialWrite) {
            // What's left of it went out ahead of this command, without its CR: end that line,
            // and drop whatever the modem makes of it
            partialWrite = false;
            write("\r", steady_clock::now() + PROBE_TIMEOUT);
            Task::delay(PROBE_TIMEOUT);
            discardInput();
        }
        settle(command);
        auto deadline = steady_clock::now() + timeout;
        {
            std::scoped_lock lock(mutex);
            pending.emplace(command);
            xSemaphoreTake(responseDone, 0);
        }
        bool written = write(command + "\r", deadline);
        if (written) {
            auto left = std::max(duration_cast<milliseconds>(deadline - steady_clock::now()), 0ms);
            xSemaphoreTake(responseDone, pdMS_TO_TICKS(left.count()));
        } else {
            LOGTD(CELLULAR, "%s: the modem isn't taking data (CTS high)", command.c_str());
            partialWrite = true;
        }
        std::scoped_lock lock(mutex);
        // Only command() sets and resets it, and the caller sends one command at a time
        AtResponse response { .result = AtResult::Timeout, .lines = {}, .error = {} };
        if (pending) {
            response = pending->collector.take();
        }
        pending.reset();
        // Checked under the lock: the result may have come in just after the wait ended
        if (written && response.result == AtResult::Timeout) {
            owed++;
        }
        return response;
    }

    /**
     * @brief Changes the UART's rate, once what's still going out has gone.
     */
    void setBaudRate(int rate) {
        uart_wait_tx_done(port, pdMS_TO_TICKS(100));
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_baudrate(port, static_cast<uint32_t>(rate)));
    }

    /**
     * @brief Drops whatever the modem sent that hasn't been read yet, and any result still owed
     * by a command that timed out: after a baud rate switch or a reset, that's garbage.
     */
    void discardInput() {
        uart_flush_input(port);
        // A line the garbage started, too
        discardLine = true;
        std::scoped_lock lock(mutex);
        owed = 0;
    }

    uint32_t getErrorCount() const {
        return errors.load();
    }

private:
    AtChannel(uart_port_t port, QueueHandle_t events, UrcPredicate isUrc)
        : port(port)
        , events(events)
        , isUrc(std::move(isUrc))
        , responseDone(xSemaphoreCreateBinary()) {
        if (responseDone == nullptr) {
            // Only fails when out of memory, at boot
            esp_system_abort("could not create AT channel");
        }
        // The URC handlers run here too, logging
        Task::run("cellular-uart", 6144, [this](Task& /*task*/) {
            readLoop();
        });
    }

    struct PendingCommand {
        explicit PendingCommand(const std::string& command)
            : command(command)
            , collector(command) {
        }

        const std::string command;
        AtResponseCollector collector;
    };

    /**
     * @brief Puts the line into the hardware FIFO as it empties, until the deadline.
     *
     * With flow control, a modem that stops taking data (in reset, or wedged) would block a
     * buffered write forever; this gives up instead.
     *
     * @return whether all of it went out
     */
    bool write(const std::string& line, steady_clock::time_point deadline) {
        size_t sent = 0;
        while (true) {
            int count = uart_tx_chars(port, line.data() + sent, line.size() - sent);
            if (count < 0) {
                return false;
            }
            sent += static_cast<size_t>(count);
            if (sent == line.size()) {
                return true;
            }
            if (steady_clock::now() >= deadline) {
                return false;
            }
            // Returns as soon as the FIFO has emptied: 1.4 ms for all 128 bytes at 921600 baud
            uart_wait_tx_done(port, 1);
        }
    }

    /**
     * @brief Waits for the final results still owed by commands that timed out.
     */
    void settle(const std::string& command) {
        auto deadline = steady_clock::now() + SETTLE_TIMEOUT;
        while (true) {
            {
                std::scoped_lock lock(mutex);
                if (owed == 0) {
                    return;
                }
                if (steady_clock::now() >= deadline) {
                    LOGTW(CELLULAR, "%s: still no answer to %d earlier command(s), sending anyway", command.c_str(), owed);
                    owed = 0;
                    return;
                }
            }
            Task::delay(10ms);
        }
    }

    [[noreturn]] void readLoop() {
        std::array<uint8_t, 512> chunk {};
        while (true) {
            uart_event_t event {};
            if (xQueueReceive(events, &event, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            switch (event.type) {
                case UART_DATA:
                // The driver's buffer filled while we were busy: with flow control, the modem
                // waits, and nothing is lost
                case UART_BUFFER_FULL:
                    // Everything buffered, not just the event's share: a chunk left behind would
                    // wait for the next event, which may be a long time coming
                    while (true) {
                        int length = uart_read_bytes(port, chunk.data(), chunk.size(), 0);
                        if (length <= 0) {
                            break;
                        }
                        // Before every chunk: discardInput() may have come in the middle of these
                        if (discardLine.exchange(false)) {
                            lines.reset();
                        }
                        notifyActivity();
                        feed(std::string_view(reinterpret_cast<const char*>(chunk.data()), static_cast<size_t>(length)));
                    }
                    break;
                case UART_FIFO_OVF:
                    // Shouldn't happen with flow control; what's buffered can't be trusted then
                    errors++;
                    LOGTW(CELLULAR, "UART receive overflow from the modem");
                    uart_flush_input(port);
                    xQueueReset(events);
                    break;
                case UART_FRAME_ERR:
                case UART_PARITY_ERR:
                    errors++;
                    LOGTW(CELLULAR, "UART %s error from the modem", event.type == UART_FRAME_ERR ? "framing" : "parity");
                    break;
                default:
                    break;
            }
        }
    }

    void notifyActivity() {
        ActivityHandler handler;
        {
            std::scoped_lock lock(mutex);
            handler = activityHandler;
        }
        if (handler) {
            handler();
        }
    }

    void feed(std::string_view data) {
        auto overlong = lines.getOverlongCount();
        lines.feed(data, [this](std::string_view line) {
            route(line);
        });
        if (lines.getOverlongCount() != overlong) {
            errors++;
            LOGTW(CELLULAR, "Dropped the rest of a line from the modem longer than %zu bytes", MAX_LINE);
        }
    }

    void route(std::string_view line) {
        UrcHandler handler;
        {
            std::scoped_lock lock(mutex);
            if (pending && !isUrc(line, pending->command)) {
                if (pending->collector.feed(line)) {
                    xSemaphoreGive(responseDone);
                }
                return;
            }
            if (!pending && owed > 0 && isFinalResultCode(line)) {
                // The late end of a command that timed out
                LOGTV(CELLULAR, "Late result from the modem: %.*s", static_cast<int>(line.size()), line.data());
                owed--;
                return;
            }
            handler = urcHandler;
        }
        if (handler) {
            handler(line);
        }
    }

    // A hex socket read of 2048 bytes is a 4 KB line
    static constexpr size_t MAX_LINE = 8192;
    // Enough for a full socket read and the URCs around it, while the reader catches up
    static constexpr int RX_BUFFER_SIZE = 16384;
    static constexpr int EVENT_QUEUE_SIZE = 32;
    // How long the modem gets to answer the CR that ends a partly sent command
    static constexpr milliseconds PROBE_TIMEOUT = 100ms;
    // How long the next command waits for a timed-out one to finish
    static constexpr milliseconds SETTLE_TIMEOUT = 10s;

    const uart_port_t port;
    QueueHandle_t events;
    const UrcPredicate isUrc;
    const SemaphoreHandle_t responseDone;
    std::atomic<uint32_t> errors { 0 };
    // Whether the last command only partly went out; only touched by command()
    bool partialWrite = false;
    // Set by discardInput(), for the reader to drop the line it has started
    std::atomic<bool> discardLine { false };

    // Only touched on the reader task
    AtLineAssembler lines { MAX_LINE };

    // Guards everything below
    std::mutex mutex;
    std::optional<PendingCommand> pending;
    // Final results still to come from commands that timed out
    int owed = 0;
    UrcHandler urcHandler;
    ActivityHandler activityHandler;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
