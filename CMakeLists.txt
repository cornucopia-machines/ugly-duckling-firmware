cmake_minimum_required(VERSION 3.16.0)

include(DeviceCommon.cmake)

project(ugly-duckling)

# This has to be here in order for the ESP-IDF VSCode extension to recognize the project as an ESP-IDF project
# See https://github.com/espressif/vscode-esp-idf-extension/issues/1756
# include($ENV{IDF_PATH}/tools/cmake/project.cmake)

add_custom_command(
    TARGET app POST_BUILD
    COMMAND ${PYTHON} ${CMAKE_SOURCE_DIR}/generate-clang-compile-db.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Regenerating clang compile DB"
    VERBATIM
)
