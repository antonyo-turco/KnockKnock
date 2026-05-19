cmake_minimum_required(VERSION 3.16.0)

# Register all component directories so the IDF build system finds them.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/components/secure_store"
    "${CMAKE_SOURCE_DIR}/components/espnow_manager"
    "${CMAKE_SOURCE_DIR}/components/serial_bridge"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gateway)
