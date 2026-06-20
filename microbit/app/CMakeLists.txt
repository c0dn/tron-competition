file(GLOB app_children RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "*")
list(SORT app_children)

foreach(app_child IN LISTS app_children)
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${app_child}" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${app_child}/CMakeLists.txt")
    add_subdirectory("${app_child}")
  endif()
endforeach()
