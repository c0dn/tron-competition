# ble_sniffer - standalone ESP-IDF project, built and flashed on its own.
#
# Not a build variant of the sidecar and not linked against it: the only thing
# shared with the rest of the repo is the wire contract in shared/, pulled in as
# an include path by main/CMakeLists.txt.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ble_sniffer)
