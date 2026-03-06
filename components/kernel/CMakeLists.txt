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
        esp_event
        esp_http_client
        esp_https_ota
        esp_netif
        esp_timer
        esp_wifi
        espcoredump
        mbedtls
        mqtt
        nvs_flash
        ulp
        wifi_provisioning

        # ESP-IDF-lib components
        bh1750
        ds18x20
        i2cdev
        ina219
        sht3x
        si7021
        tsl2591

        # ArduinoJSON
        bblanchon__arduinojson
        kode_bq27220
)

set_target_properties(${COMPONENT_LIB} PROPERTIES WHOLE_ARCHIVE 1)

set(ulp_app_name ulp_pulse_counter)
set(ulp_sources "${CMAKE_CURRENT_LIST_DIR}/ulp/pulse_counter.c")
# PulseCounter.cpp includes the generated ulp_pulse_counter.h, so it must not
# be compiled until the ULP binary (and its generated header) are ready.
set(ulp_exp_dep_srcs "src/PulseCounter.cpp")

ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
