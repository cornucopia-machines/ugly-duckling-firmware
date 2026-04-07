cmake_minimum_required(VERSION 3.16)

project(ugly-duckling-unit-tests CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Disable building tests for external dependencies
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

# Fetch Catch2
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.0
)
FetchContent_MakeAvailable(Catch2)

# Fetch ArduinoJson
FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.2.0
)
FetchContent_MakeAvailable(ArduinoJson)

# Include directories
set(COMPONENTS_DIR ${CMAKE_SOURCE_DIR}/../../components)

include_directories(
    ${COMPONENTS_DIR}/kernel/src
    ${COMPONENTS_DIR}/kernel/test
    ${COMPONENTS_DIR}/utils/src
    ${COMPONENTS_DIR}/peripherals-api/src
    ${COMPONENTS_DIR}/scheduling/src
    ${COMPONENTS_DIR}/scheduling/test
    ${COMPONENTS_DIR}/utils/src
    ${COMPONENTS_DIR}/test-support/src
)

# Collect all test source files
file(GLOB_RECURSE TEST_SOURCES
    ${COMPONENTS_DIR}/kernel/test/*.cpp
    ${COMPONENTS_DIR}/scheduling/test/*.cpp
)

# Collect component source files (excluding tests and ESP-IDF specific code)
set(COMPONENT_SOURCES

    # kernel/State.cpp excluded as it depends on FreeRTOS
)

# Create the test executable
add_executable(ugly-duckling-unit-tests
    main/test_main.cpp
    ${TEST_SOURCES}
    ${COMPONENT_SOURCES}
)

# Link libraries
target_link_libraries(ugly-duckling-unit-tests
    PRIVATE
    Catch2::Catch2
    ArduinoJson
)

# Copy MinGW runtime DLLs to build directory on Windows
if(WIN32 AND (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    # Find the compiler's bin directory
    get_filename_component(COMPILER_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)

    # Copy required DLLs after build
    add_custom_command(TARGET ugly-duckling-unit-tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${COMPILER_BIN_DIR}/libgcc_s_seh-1.dll"
            "${COMPILER_BIN_DIR}/libstdc++-6.dll"
            "${COMPILER_BIN_DIR}/libwinpthread-1.dll"
            $<TARGET_FILE_DIR:ugly-duckling-unit-tests>
        COMMENT "Copying MinGW runtime DLLs..."
    )
endif()

# Enable testing
enable_testing()
add_test(NAME UnitTests COMMAND ugly-duckling-unit-tests)

include(CTest)
include(Catch)
catch_discover_tests(ugly-duckling-unit-tests)
