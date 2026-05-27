cmake_minimum_required(VERSION 3.16.0)

# Register src directory as component
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/src"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gateway)
