cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include(../../DeviceCommon.cmake)

# Append test-specific sdkconfig overrides (after DeviceCommon sets SDKCONFIG_DEFAULTS)
set(SDKCONFIG_DEFAULTS "${SDKCONFIG_DEFAULTS};${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

project(ugly-duckling-embedded-tests)
