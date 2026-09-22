include(${CMAKE_CURRENT_LIST_DIR}/Common.cmake)

# Do not fail on warnings in ESP-IDF
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)

# Fail on warnings
add_compile_options(-Wall)
add_compile_options(-Wextra)
add_compile_options(-Wunused)
add_compile_options(-Wreturn-local-addr)
add_compile_options(-Werror)

# UD_GEN

if(NOT DEFINED UD_GEN)
    set(UD_GEN "$ENV{UD_GEN}")
endif()
string(TOLOWER "${UD_GEN}" UD_GEN_LOWER)

# Make sure we reconfigure if UD_GEN changes
set_property(GLOBAL PROPERTY UD_GEN_TRACKER "${UD_GEN}")

if(UD_GEN)
    add_compile_definitions("${UD_GEN}")
endif()

# Determine the expected IDF target for this generation
set(ESP32_S3_GENS
    MK5_REV2
    MK6_REV1 MK6_REV2 MK6_REV3
    MK7_REV1
    MK8_REV1
    MK9_REV1
)
set(ESP32_C6_GENS
    MK10_REV1
)

if(UD_GEN STREQUAL "")
    # No generation override — infer target from IDF_TARGET
    if("$ENV{IDF_TARGET}" STREQUAL "esp32s3")
        set(_ud_target "esp32s3")
        set(_ud_platform "spinach")
    elseif("$ENV{IDF_TARGET}" STREQUAL "esp32c6")
        set(_ud_target "esp32c6")
        set(_ud_platform "carrot")
    else()
        message(FATAL_ERROR "Error: IDF_TARGET='$ENV{IDF_TARGET}' is not a supported target")
    endif()
    message("Building Ugly Duckling for platform '${_ud_platform}' (runtime MAC detection)")
elseif(UD_GEN IN_LIST ESP32_S3_GENS)
    set(_ud_target "esp32s3")
    set(_ud_platform "spinach")
    message("Building Ugly Duckling '${UD_GEN}' (forced via UD_GEN)")
elseif(UD_GEN IN_LIST ESP32_C6_GENS)
    set(_ud_target "esp32c6")
    set(_ud_platform "carrot")
    message("Building Ugly Duckling '${UD_GEN}' (forced via UD_GEN)")
else()
    message(FATAL_ERROR "Error: Unrecognized Ugly Duckling generation '${UD_GEN}'")
endif()

# IDF_TARGET checks

if(NOT "$ENV{IDF_TARGET}" STREQUAL "${_ud_target}")
    message(FATAL_ERROR "Error: IDF_TARGET='$ENV{IDF_TARGET}' does not match expected target '${_ud_target}'")
endif()

if(IDF_TARGET STREQUAL "esp32s3")
    add_compile_options(-mtext-section-literals) # To fix 'literal target out of range' errors
endif()

# UD_DEBUG

if(NOT DEFINED UD_DEBUG)
    set(UD_DEBUG "$ENV{UD_DEBUG}")
endif()
if(UD_DEBUG STREQUAL "")
    message("UD_DEBUG is not set, assuming 0.")
    set(UD_DEBUG 0)
endif()

if(NOT DEFINED UD_LOG_VERBOSE)
    set(UD_LOG_VERBOSE "$ENV{UD_LOG_VERBOSE}")
endif()

if(UD_DEBUG)
    add_compile_definitions(UD_DEBUG)
    add_compile_definitions(UD_LOG_VERBOSE="${UD_LOG_VERBOSE}")
    add_compile_definitions(DUMP_MQTT)
endif()

# UD_DEBUG_CONSOLE — separate from UD_DEBUG because the two want different terminals. It draws
# a status line the log output then has to erase and redraw around, which is only readable on an
# interactive terminal; anything that captures the serial output to a file or scrapes it (CI,
# Wokwi runs, `idf.py monitor | tee`) ends up with cursor escapes interleaved into every line.
# UD_DEBUG on its own gives the verbose logging without any of that.

if(NOT DEFINED UD_DEBUG_CONSOLE)
    set(UD_DEBUG_CONSOLE "$ENV{UD_DEBUG_CONSOLE}")
endif()
if(UD_DEBUG_CONSOLE STREQUAL "")
    set(UD_DEBUG_CONSOLE 0)
endif()

if(UD_DEBUG_CONSOLE)
    add_compile_definitions(UD_DEBUG_CONSOLE)
endif()

# UD_NOSLEEP — keep the device out of light sleep (see PowerManager::shouldSleepWhenIdle).
#
# Off by default, including for UD_DEBUG builds: those used to skip light sleep unconditionally,
# which meant sleep-related bugs vanished the moment you built with debug output.

if(NOT DEFINED UD_NOSLEEP)
    set(UD_NOSLEEP "$ENV{UD_NOSLEEP}")
endif()
if(UD_NOSLEEP STREQUAL "")
    set(UD_NOSLEEP 0)
endif()

# Forced on rather than merely defaulted on by the status line, because the two cannot coexist:
# a light-sleeping device wakes only for real work, so a 4 Hz redraw either drags it awake
# around the clock -- making the measurements on the status line describe a device that only
# looks like that because you are watching it -- or the line sits stale between wakeups.
if(UD_DEBUG_CONSOLE AND NOT UD_NOSLEEP)
    message("UD_DEBUG_CONSOLE implies UD_NOSLEEP")
    set(UD_NOSLEEP 1)
endif()

if(UD_NOSLEEP)
    message("Building with light sleep disabled")
    add_compile_definitions(UD_NOSLEEP)
endif()

# UD_PM_DIAGNOSTICS — separate from UD_DEBUG so the diagnostics can be built with debug logging.
# Do not combine it with UD_NOSLEEP: a device that never light-sleeps has nothing to report.

if(NOT DEFINED UD_PM_DIAGNOSTICS)
    set(UD_PM_DIAGNOSTICS "$ENV{UD_PM_DIAGNOSTICS}")
endif()
if(UD_PM_DIAGNOSTICS STREQUAL "")
    set(UD_PM_DIAGNOSTICS 0)
endif()

if(UD_PM_DIAGNOSTICS)
    add_compile_definitions(UD_PM_DIAGNOSTICS)
endif()

# WOKWI

if(NOT DEFINED WOKWI)
    set(WOKWI "$ENV{WOKWI}")
endif()

if(NOT DEFINED WOKWI_MQTT_HOST)
    set(WOKWI_MQTT_HOST "$ENV{WOKWI_MQTT_HOST}")
endif()

# Make sure we reconfigure if parameters change
set_property(DIRECTORY PROPERTY WOKWI_TRACKER "${WOKWI} ${WOKWI_MQTT_HOST}")

if(WOKWI)
    add_compile_definitions(WOKWI)
    if (WOKWI_MQTT_HOST)
        add_compile_definitions(WOKWI_MQTT_HOST="${WOKWI_MQTT_HOST}")
    endif()
endif()

# Make sure we reconfigure if any of the debug parameters change
set_property(DIRECTORY PROPERTY UD_DEBUG_TRACKER "${UD_DEBUG} ${UD_DEBUG_CONSOLE} ${UD_NOSLEEP} ${UD_PM_DIAGNOSTICS}")

set(SDKCONFIG_FILES)
list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${_ud_platform}.defaults")

if (UD_PM_DIAGNOSTICS)
    message("Building with PM diagnostics")
    list(APPEND SDKCONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.pm_diagnostics.defaults")
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

add_compile_definitions(UD_PLATFORM="${_ud_platform}")

add_link_options("-Wl,--gc-sections")
