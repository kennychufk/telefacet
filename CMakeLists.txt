cmake_minimum_required(VERSION 3.20)
project(telefacet LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

option(TELEFACET_BUILD_GUI   "Build the GLFW/ImGui GUI executable" ON)
option(TELEFACET_BUILD_TESTS "Build the headless end-to-end test binary" OFF)
option(TELEFACET_BUILD_TOOLS "Build the headless diagnostic tools in tools/" OFF)

find_package(Threads REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# ----- libtelefacet_core — GUI-free protocol/networking/config library -----
# Anything that doesn't depend on GLFW/ImGui/OpenGL. The GUI target and the
# headless test binary both link against this.
add_library(telefacet_core STATIC
  src/config/ConfigLoader.cpp
  src/net/ChunkReassembler.cpp
  src/net/WebSocketClient.cpp
  src/net/MultiServerManager.cpp
  src/data/FrameBufferPool.cpp
  src/data/CameraStore.cpp
  src/client/Client.cpp
)

target_include_directories(telefacet_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(telefacet_core PUBLIC
  ixwebsocket
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  Threads::Threads
)

# ----- GUI executable -----
if(TELEFACET_BUILD_GUI)
  # Embed shader sources as C++ string literals at configure time.
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debayer.vert" DEBAYER_VERT_SRC)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/shaders/debayer.frag" DEBAYER_FRAG_SRC)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbeddedShaders.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/telefacet/EmbeddedShaders.hpp"
    @ONLY
  )

  add_executable(telefacet
    src/main.cpp
    src/app/App.cpp
    src/gl/Shader.cpp
    src/gl/YuvRenderer.cpp
    src/ui/CameraView.cpp
    src/ui/CameraGrid.cpp
    src/ui/ControlPanel.cpp
  )

  target_include_directories(telefacet PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/generated
  )

  find_package(OpenGL REQUIRED)
  target_link_libraries(telefacet PRIVATE
    telefacet_core
    glfw
    glad_gl_core_33
    imgui
    OpenGL::GL
    ${CMAKE_DL_LIBS}
  )
endif()

# ----- Headless diagnostic tools -----
if(TELEFACET_BUILD_TOOLS)
  add_executable(lowfps_repro tools/lowfps_repro.cpp)
  target_link_libraries(lowfps_repro PRIVATE telefacet_core)
endif()

# ----- Headless end-to-end tests -----
if(TELEFACET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
