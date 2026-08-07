# One interface target per hardware capability. Apps link only what they use;
# source files and this include directory then propagate to the firmware target.

function(add_microbit_driver name source)
  add_library(microbit_driver_${name} INTERFACE)
  target_sources(microbit_driver_${name} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/${source}
  )
  target_include_directories(microbit_driver_${name} INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
  )
endfunction()

add_microbit_driver(ble_radio ble_radio.c)
add_microbit_driver(display   display.c)
add_microbit_driver(imu       imu.c)
add_microbit_driver(mic       mic.c)
