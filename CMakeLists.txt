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

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Dependencies.cmake)

# ----- Embed shader sources as C++ string literals at configure time -----
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
  src/config/ConfigLoader.cpp
  src/net/ChunkReassembler.cpp
  src/net/WebSocketClient.cpp
  src/net/MultiServerManager.cpp
  src/data/FrameBufferPool.cpp
  src/data/CameraStore.cpp
  src/gl/Shader.cpp
  src/gl/YuvRenderer.cpp
  src/ui/CameraView.cpp
  src/ui/CameraGrid.cpp
  src/ui/ControlPanel.cpp
)

target_include_directories(telefacet PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(telefacet PRIVATE
  glfw
  glad_gl_core_33
  imgui
  ixwebsocket
  yaml-cpp::yaml-cpp
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  Threads::Threads
)

find_package(Threads REQUIRED)
find_package(OpenGL REQUIRED)
target_link_libraries(telefacet PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
