list(APPEND lib_telefacet_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/shader.cpp
)

add_library(lib_telefacet ${lib_telefacet_SOURCES})

target_link_libraries(lib_telefacet
  PUBLIC 
    fltk::gl
)

target_include_directories(lib_telefacet
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
