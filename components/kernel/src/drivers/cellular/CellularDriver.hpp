#pragma once

#include <Log.hpp>
#include <PowerManager.hpp>
#include <Task.hpp>
#include <drivers/cellular/Bc660KDriver.hpp>
#include <drivers/cellular/CellularModemPins.hpp>
#include <drivers/cellular/CellularModuleDriver.hpp>
#include <drivers/cellular/Cereg.hpp>
#include <drivers/cellular/EspModem.hpp>

#include <sdkconfig.h>
#include <soc/uart_pins.h>

#include <chrono>
#include <exception>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::drivers::cellular {

/**
 * @brief Owns the cellular modem: its UART, bring-up, and (later) registration and networking.
 *
 * Stage 2 of docs/specs/NB-IoT.md: talk to the modem, configure it, and log what it says.
 * Everything chipset-specific goes through CellularModuleDriver.
 *
 * The modem is optional hardware, so nothing here is fatal: if the modem's pins are taken by the
 * console, or it does not answer, the driver logs why and the device carries on without it.
 */
class CellularDriver {
public:
    explicit CellularDriver(const CellularModemPins& pins)
        : pins(pins) {
        Task::run("cellular", 4096, [this](Task& /*task*/) {
            try {
                run();
            } catch (const std::exception& e) {
                LOGTE(CELLULAR, "Cellular modem failed: %s", e.what());
                // Don't keep the device out of light sleep for a modem we've given up on
                noLightSleepGuard.reset();
            }
        });
    }

private:
    void run() {
        if (sharesPinsWithConsole(pins)) {
            // UD_UART0_CONSOLE builds put the console back on these pins for early-boot debugging
            LOGTW(CELLULAR, "Console is on UART0, which uses the modem's pins (GPIO %d/%d); not starting the modem",
                static_cast<int>(pins.tx->getGpio()), static_cast<int>(pins.rx->getGpio()));
            return;
        }

        LOGTI(CELLULAR, "Starting modem on UART%d (TX GPIO %d, RX GPIO %d)",
            static_cast<int>(MODEM_UART), static_cast<int>(pins.tx->getGpio()), static_cast<int>(pins.rx->getGpio()));

        // The UART driver only holds its PM lock while transmitting, so in light sleep anything the
        // modem sends -- the reply to a command we are waiting on, or a URC -- would be lost. Stay
        // awake for as long as the modem is up; waking on UART activity instead belongs with
        // putting the modem itself to sleep (docs/specs/NB-IoT.md, stage 4).
        noLightSleepGuard = std::make_unique<PowerManagementLockGuard>(noLightSleep);

        auto dte = createDte(pins);
        module = std::make_shared<Bc660KDriver>(dte);
        module->onUrc([](std::string_view line) {
            handleUrc(line);
        });

        if (!module->wake()) {
            LOGTE(CELLULAR, "%s is not answering; is the daughter board connected?", module->getName());
            noLightSleepGuard.reset();
            return;
        }
        LOGTI(CELLULAR, "%s is answering", module->getName());

        if (!module->configure()) {
            LOGTW(CELLULAR, "Some %s settings could not be applied", module->getName());
        }
        module->logStatus();
        monitorRegistration();
    }

    /**
     * @brief Polls registration, since +CEREG URCs only report changes: a modem that keeps
     * searching would otherwise never say so.
     */
    [[noreturn]] void monitorRegistration() {
        bool wasRegistered = false;
        std::optional<RegistrationStatus> lastStatus;
        while (true) {
            auto registration = queryRegistration();
            bool registered = registration && registration->isRegistered();
            if (!registration) {
                LOGTW(CELLULAR, "Could not read the registration status");
            } else if (!registered || registration->status != lastStatus) {
                LOGTI(CELLULAR, "Network: %s", describe(*registration).c_str());
            }
            if (registered && !wasRegistered) {
                logNetworkDetails();
            }
            if (!registered || !wasRegistered) {
                module->logRadioStatus();
            }
            if (!registered) {
                // Automatic or manual operator selection, and the operator if there is one
                logQuery("AT+COPS?");
            }
            wasRegistered = registered;
            lastStatus = registration ? std::optional(registration->status) : std::nullopt;
            Task::delay(registered ? REGISTERED_POLL_INTERVAL : SEARCHING_POLL_INTERVAL);
        }
    }

    std::optional<Registration> queryRegistration() {
        auto response = module->command("AT+CEREG?", 5s);
        for (const auto& line : response.lines) {
            if (auto registration = parseCeregRead(line)) {
                return registration;
            }
        }
        return std::nullopt;
    }

    void logNetworkDetails() {
        // Operator, packet-domain attach, and the PDP context the network handed out (APN, IP)
        for (const char* query : { "AT+COPS?", "AT+CGATT?", "AT+CGDCONT?" }) {
            logQuery(query);
        }
    }

    void logQuery(const char* query) {
        auto response = module->command(query, 5s);
        if (!response.ok()) {
            LOGTW(CELLULAR, "%s: %s %s", query, toString(response.result), response.error.c_str());
            return;
        }
        for (const auto& line : response.lines) {
            LOGTI(CELLULAR, "%s: %s", query, line.c_str());
        }
    }

    static std::shared_ptr<esp_modem::DTE> createDte(const CellularModemPins& pins) {
        esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();
        config.uart_config.port_num = MODEM_UART;
        config.uart_config.tx_io_num = pins.tx->getGpio();
        config.uart_config.rx_io_num = pins.rx->getGpio();
        config.uart_config.rts_io_num = UART_PIN_NO_CHANGE;
        config.uart_config.cts_io_num = UART_PIN_NO_CHANGE;
        // 115200 is far above what NB-IoT delivers (docs/specs/NB-IoT.md, "UART baud rate")
        config.uart_config.baud_rate = 115200;
        // Room for a full AT+QIRD read (512 bytes of data plus framing) once sockets come along
        config.dte_buffer_size = 1024;
        auto dte = esp_modem::create_uart_dte(&config);
        if (dte == nullptr) {
            throw std::runtime_error("could not create UART terminal");
        }
        return dte;
    }

    static void handleUrc(std::string_view line) {
        if (auto registration = parseCeregUrc(line)) {
            LOGTI(CELLULAR, "Network: %s", describe(*registration).c_str());
            return;
        }
        LOGTD(CELLULAR, "URC: %.*s", static_cast<int>(line.size()), line.data());
    }

    static bool sharesPinsWithConsole([[maybe_unused]] const CellularModemPins& pins) {
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART_NUM == 0
        auto isConsolePin = [](const InternalPinPtr& pin) {
            return pin->getGpio() == U0TXD_GPIO_NUM || pin->getGpio() == U0RXD_GPIO_NUM;
        };
        return isConsolePin(pins.tx) || isConsolePin(pins.rx);
#else
        return false;
#endif
    }

    static constexpr milliseconds SEARCHING_POLL_INTERVAL = 30s;
    static constexpr milliseconds REGISTERED_POLL_INTERVAL = 5min;

    // UART0 is the console's when UD_UART0_CONSOLE is set; UART1 works on any pin via the GPIO matrix
    static constexpr uart_port_t MODEM_UART = UART_NUM_1;

    const CellularModemPins pins;
    std::shared_ptr<CellularModuleDriver> module;

    PowerManagementLock noLightSleep { "cellular", ESP_PM_NO_LIGHT_SLEEP };
    std::unique_ptr<PowerManagementLockGuard> noLightSleepGuard;
};

}    // namespace cornucopia::ugly_duckling::kernel::drivers::cellular
