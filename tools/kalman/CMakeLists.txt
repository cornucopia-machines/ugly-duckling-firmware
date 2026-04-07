cmake_minimum_required(VERSION 3.16.0)

project(kalman_cli LANGUAGES CXX)

# Ensure we build with a modern standard
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Resolve repository root to add include path for components/utils
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_executable(kalman_cli
    main.cpp
)

target_include_directories(kalman_cli PRIVATE
    "${REPO_ROOT}/components/utils"
)

if (MINGW)
  target_link_options(kalman_cli PRIVATE -static-libstdc++ -static-libgcc)
endif()
