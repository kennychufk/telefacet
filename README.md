# telefacet

Native C++ / OpenGL replacement for the `telefacet-web` Vue client. Connects
to one or more `cherupi-v4l2` WebSocket servers (WebSocket protocol v6),
converts the YUV420 frames delivered by the server's hardware ISP to RGB on
the GPU, and renders them in a dockable multi-camera view. Detector process modes
(`checkerboard` / `checkerboard2x2` / `aruco` / `aruco2x2`) overlay their
per-frame detections on each camera view — checkerboard corners in green,
ArUco/AprilTag markers as amber quads with corner dots and id labels.

## Build (Linux)

```sh
cmake -B build -S .
cmake --build build -j
```

All third-party dependencies (GLFW, glad, Dear ImGui docking branch,
ixwebsocket, yaml-cpp, nlohmann/json, spdlog) are fetched automatically by
CMake at configure time. The shader sources in `shaders/` are embedded into
the binary at configure time, so the executable is self-contained.

## Run

```sh
./build/telefacet configs/example.yaml
```

`telefacet` connects, runs `discover`, and waits. Use the **telefacet**
control panel to `Configure`, `Start cameras`, then toggle individual
streams. Press **P** to hide/show the control panel.

## Layout

* `src/net/` — WebSocket client, chunked-frame reassembly, multi-server roster
* `src/data/` — `FrameBufferPool` (recycled byte buffers) and `CameraStore`
  (latest-frame-per-camera, atomic publish/consume)
* `src/gl/` — `Shader` helper and the `Debayer` GL pass (port of
  `telefacet-web/src/webgl/Debayer.js`)
* `src/ui/` — `CameraView` (per-camera ImGui window), `CameraGrid`
  (DockSpace), `ControlPanel` (commands)
* `src/app/` — `App` glue, GLFW + ImGui bring-up

## Threading

* Each `WebSocketClient` runs its own background thread (provided by
  ixwebsocket). It parses chunks, assembles complete frames in the network
  thread, and publishes to `CameraStore` via an atomic latest-frame slot.
* The GL/main thread polls `CameraStore::consumeIfNew` per camera, uploads
  to a persistent `GL_R8` texture, runs the debayer shader into a per-camera
  FBO, and draws the result via `ImGui::Image`.
* Latest-wins semantics — older frames are dropped (matches the live-viewer
  behavior of the JS client).
