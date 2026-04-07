#pragma once

#include <Configuration.hpp>
#include <Pin.hpp>

using namespace farmhub::kernel;

namespace farmhub::devices {

struct DeviceSettings : ConfigurationSection {
    ArrayProperty<JsonAsString> peripherals { this, "peripherals" };
    ArrayProperty<JsonAsString> functions { this, "functions" };

    Property<bool> sleepWhenIdle { this, "sleepWhenIdle", true };

    /**
     * @brief How often to publish telemetry.
     */
    Property<seconds> publishInterval { this, "publishInterval", 5min };
    Property<Level> publishLogs { this, "publishLogs",
#ifdef FARMHUB_DEBUG
        Level::Verbose
#else
        Level::Info
#endif
    };

    /**
     * @brief How long without successfully published telemetry before the watchdog times out and reboots the device.
     */
    Property<seconds> watchdogTimeout { this, "watchdogTimeout", 15min };

    /**
     * @brief Om the MK6 the built-in motor driver's nSLEEP pin can be manually set by a jumper,
     * but can be connected to a GPIO pin, too. Defaults to C2 on Rev1 and Rev2,
     * and to LOADEN on Rev3+.
     * @note Only relevant for MK6 Rev1 and Rev2.
     */
    Property<PinPtr> motorNSleepPin { this, "motorNSleepPin" };
};

}    // namespace farmhub::devices
