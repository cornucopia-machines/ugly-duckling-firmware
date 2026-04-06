idf_component_register(
    SRCS
        src/drivers/MotorDriver.cpp
        src/State.cpp
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
