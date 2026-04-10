idf_component_register(
    SRC_DIRS ${CMAKE_CURRENT_LIST_DIR}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}
    EMBED_TXTFILES "${CMAKE_SOURCE_DIR}/partitions.csv"
    PRIV_REQUIRES
        functions
        devices
)

# Generate the NVS config partition. Use `idf.py flash` to upload it along
# with the firmware, or `idf.py app-flash` to upload only the app.
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
