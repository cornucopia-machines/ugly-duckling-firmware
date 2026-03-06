include(${CMAKE_CURRENT_LIST_DIR}/Common.cmake)

if(NOT DEFINED UD_GEN)
    set(UD_GEN "$ENV{UD_GEN}")
endif()
if(UD_GEN STREQUAL "")
    message("UD_GEN is not set, assuming MK7.")
    set(UD_GEN MK7)
endif()
string(TOLOWER "${UD_GEN}" UD_GEN_LOWER)

# Make sure we reconfigure if UD_GEN changes
set_property(GLOBAL PROPERTY UD_GEN_TRACKER "${UD_GEN}")

set(ESP32_S3_GENS MK5 MK6 MK7 MK8)
set(ESP32_C6_GENS MKX)
set(_ud_gen_target)
if(UD_GEN IN_LIST ESP32_S3_GENS)
    set(_ud_gen_target "esp32s3")
elseif(UD_GEN IN_LIST ESP32_C6_GENS)
    set(_ud_gen_target "esp32c6")
else()
    message(FATAL_ERROR "Error: Unrecognized Ugly Duckling generation '${UD_GEN}'")
endif()

if(NOT "$ENV{IDF_TARGET}" STREQUAL "${_ud_gen_target}")
    message(FATAL_ERROR "Error: Ugly Duckling ${UD_GEN} is only supported on ${_ud_gen_target}, currently IDF_TARGET = '$ENV{IDF_TARGET}'")
endif()

message("Building Ugly Duckling '${UD_GEN}'")

if(NOT DEFINED UD_DEBUG)
    set(UD_DEBUG "$ENV{UD_DEBUG}")
endif()
if(UD_DEBUG STREQUAL "")
    message("UD_DEBUG is not set, assuming 0.")
    set(UD_DEBUG 0)
endif()

# Make sure we reconfigure if UD_DEBUG changes
set_property(DIRECTORY PROPERTY UD_DEBUG_TRACKER "${UD_DEBUG}")

set(SDKCONFIG_FILES)
list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(UD_GEN IN_LIST ESP32_S3_GENS)
    list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.s3.defaults")
    list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${UD_GEN_LOWER}.defaults")
elseif(UD_GEN IN_LIST ESP32_C6_GENS)
    list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.c6.defaults")
endif()

# Check if UD_DEBUG is defined
if (UD_DEBUG)
    message("Building with debug options")
    # Add the debug-specific SDKCONFIG file to the list
    list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.debug.defaults")
    set(CMAKE_BUILD_TYPE Debug)
else()
    message("Building with release options")
    set(CMAKE_BUILD_TYPE Release)
endif()

list(JOIN SDKCONFIG_FILES ";" SDKCONFIG_DEFAULTS)

add_link_options("-Wl,--gc-sections")
