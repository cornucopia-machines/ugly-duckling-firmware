idf_component_register(
    SRC_DIRS ${CMAKE_CURRENT_LIST_DIR}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}
    EMBED_TXTFILES "${CMAKE_SOURCE_DIR}/partitions.csv"
    PRIV_REQUIRES
        functions
        devices
)

# Do not fail on warnings in ESP-IDF
idf_build_set_property(COMPILE_OPTIONS "-Wno-error" APPEND)

component_compile_options(-Wall)
component_compile_options(-Wextra)
component_compile_options(-Wunused)
component_compile_options(-Wreturn-local-addr)
component_compile_options(-Werror)

if(IDF_TARGET STREQUAL "esp32s3")
    component_compile_options(-mtext-section-literals) # To fix 'literal target out of range' errors
endif()

if(NOT DEFINED WOKWI)
    set(WOKWI "$ENV{WOKWI}")
endif()

if(NOT DEFINED WOKWI_MQTT_HOST)
    set(WOKWI_MQTT_HOST "$ENV{WOKWI_MQTT_HOST}")
endif()

# Make sure we reconfigure if parameters change
set_property(DIRECTORY PROPERTY WOKWI_TRACKER "${WOKWI} ${WOKWI_MQTT_HOST}")

if(WOKWI)
    component_compile_definitions(WOKWI)
    if (WOKWI_MQTT_HOST)
        component_compile_definitions(WOKWI_MQTT_HOST="${WOKWI_MQTT_HOST}")
    endif()
endif()

if(UD_GEN)
    component_compile_definitions("${UD_GEN}")
endif()

if(NOT DEFINED FARMHUB_LOG_VERBOSE)
    set(FARMHUB_LOG_VERBOSE "$ENV{FARMHUB_LOG_VERBOSE}")
endif()

if(UD_DEBUG)
    component_compile_definitions(FARMHUB_DEBUG)
    component_compile_definitions(FARMHUB_LOG_VERBOSE="${FARMHUB_LOG_VERBOSE}")
    component_compile_definitions(DUMP_MQTT)
endif()

# Use `idf.py -DFSUPLOAD=1 flash` to upload the NVS config partition
if(DEFINED FSUPLOAD AND FSUPLOAD)
    set(CONFIG_NVS_BIN "${CMAKE_BINARY_DIR}/config.bin")
    # NVS partition size (must match partitions.csv)
    set(CONFIG_NVS_SIZE "0x40000")

    set(GEN_CONFIG_NVS_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/gen_config_nvs.py")
    file(GLOB CONFIG_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/config/*.json")
    add_custom_command(
        OUTPUT ${CONFIG_NVS_BIN}
        COMMAND ${PYTHON} ${GEN_CONFIG_NVS_SCRIPT}
            ${CMAKE_SOURCE_DIR}/config
            ${CONFIG_NVS_BIN}
            ${CONFIG_NVS_SIZE}
        DEPENDS
            ${CONFIG_FILES}
            ${GEN_CONFIG_NVS_SCRIPT}
        COMMENT "Generating NVS config partition"
    )
    add_custom_target(config_nvs_image ALL DEPENDS ${CONFIG_NVS_BIN})

    esptool_py_flash_to_partition(flash "nvs" ${CONFIG_NVS_BIN})
endif()
