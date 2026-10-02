cmake_minimum_required(VERSION 3.16)

# Share the main firmware's lock files: this project builds the same components, so separate locks
# would only be more copies to forget to update when dependencies change. The component manager
# leaves the shared files alone only as long as both projects see the same set of idf_component.yml
# manifests (it hashes them all) -- so neither main/ nor test/e2e-tests/main/ may have one.
set(UD_DEPENDENCIES_LOCK_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")

include(../../DeviceCommon.cmake)

project(ugly-duckling-e2e-tests)
