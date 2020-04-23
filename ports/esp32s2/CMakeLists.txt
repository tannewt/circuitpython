# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include(esp-idf/tools/cmake/project.cmake)
project(circuitpython)

idf_build_set_property(__OUTPUT_SDKCONFIG 0)
