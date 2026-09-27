cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Keep the initial firmware limited to the components required by main.
idf_build_set_property(MINIMAL_BUILD ON)

project(amm_control_unit)
