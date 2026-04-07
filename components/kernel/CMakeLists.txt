file(GLOB_RECURSE KERNEL_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(
    SRCS ${KERNEL_SRCS}
    INCLUDE_DIRS
        src
    REQUIRES
        # Core ESP-IDF components
        esp_adc
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_pcnt
        esp_event
        esp_http_client
        esp_https_ota
        esp_netif
        esp_pm
        esp_timer
        esp_wifi
        espcoredump
        mbedtls
        mqtt
        network_provisioning
        nvs_flash
        ulp

        # ESP-IDF-lib components
        bh1750
        ds18x20
        i2cdev
        ina219
        sht3x
        si7021
        tsl2591

        # BQ27220 fuel gauge
        espressif__bq27220

        # ArduinoJSON
        bblanchon__arduinojson
)

set_target_properties(${COMPONENT_LIB} PROPERTIES WHOLE_ARCHIVE 1)

set(ulp_app_name ulp_pulse_counter)
set(ulp_sources "${CMAKE_CURRENT_LIST_DIR}/ulp/pulse_counter.c")
# PulseCounter.cpp includes the generated ulp_pulse_counter.h, so it must not
# be compiled until the ULP binary (and its generated header) are ready.
set(ulp_exp_dep_srcs "src/PulseCounter.cpp")

ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")

# Make the generated ULP header (ulp_kernel.h) visible to components that depend on kernel.
# ulp_embed_binary() uses directory-scoped include_directories(), which doesn't propagate.
target_include_directories(${COMPONENT_LIB} PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/${ulp_app_name})
