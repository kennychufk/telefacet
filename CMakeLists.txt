cmake_minimum_required(VERSION 3.11)

project(telefacet)

set(CMAKE_CXX_STANDARD 17)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(Boost_USE_STATIC_LIBS OFF)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.74 REQUIRED COMPONENTS system thread)
if (NOT APPLE)
  find_package(GLEW REQUIRED)
endif()

option(USE_SYSTEM_FLTK "Use system-installed FLTK instead of downloading a standalone FLTK" OFF)

include(FetchContent)

if (USE_SYSTEM_FLTK)
  find_package(FLTK 1.4 CONFIG REQUIRED)
else()
  FetchContent_Declare(
    fltk
    GIT_REPOSITORY https://github.com/fltk/fltk.git
    GIT_TAG        release-1.4.2
  )
  FetchContent_MakeAvailable(fltk)
endif()

add_subdirectory(lib)

# GLEW on Windows and macOS: https://github.com/fltk/fltk/blob/master/examples/CMakeLists.txt
add_executable(telefacet telefacet.cpp)
target_link_libraries(telefacet
  PRIVATE
    lib_telefacet
    fltk::fltk
    fltk::images
    fltk::gl
    Boost::thread
)
if (NOT APPLE)
  target_link_libraries(telefacet
    PRIVATE
      GLEW::GLEW
  )
endif()
