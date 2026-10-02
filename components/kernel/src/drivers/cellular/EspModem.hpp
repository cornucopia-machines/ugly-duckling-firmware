#pragma once

// esp_modem's headers trip the extra warnings we build our own components with (see
// DeviceCommon.cmake); include them only through here
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
#include <cxx_include/esp_modem_api.hpp>
#include <cxx_include/esp_modem_dce_module.hpp>
#include <cxx_include/esp_modem_dte.hpp>
#include <cxx_include/esp_modem_types.hpp>

#include <esp_modem_config.h>
#pragma GCC diagnostic pop
