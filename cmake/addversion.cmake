configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/plugin.h.in" "${CMAKE_CURRENT_BINARY_DIR}/cmake/plugin.h" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc" @ONLY)
target_sources("${PROJECT_NAME}" PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/cmake/version.rc ${CMAKE_CURRENT_BINARY_DIR}/cmake/plugin.h)