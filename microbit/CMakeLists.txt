cmake_minimum_required(VERSION 3.20)

project(microbit_firmware_workspace LANGUAGES C ASM)

add_subdirectory(libs/mtkernel_3)
add_subdirectory(lib)
add_subdirectory(app)
