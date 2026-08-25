# telefacet

Native C++ / OpenGL replacement for the `telefacet-web` Vue client. Connects
to one or more `cherupi-v4l2` WebSocket servers (WebSocket protocol v6),
converts the YUV420 frames delivered by the server's hardware ISP to RGB on
the GPU, and renders them in a dockable multi-camera view. Detector process modes
(`checkerboard` / `checkerboard2x2` / `aruco` / `aruco2x2`) overlay their
per-frame detections on each camera view — checkerboard corners in green,
ArUco/AprilTag markers as amber quads with corner dots and id labels. The
`trigger` process mode adds a save-on-demand shutter: nothing is written to disk
until the control panel's **Trigger capture** button (or, headlessly,
`Client::triggerCapture()`) asks for a frame.

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

### Client role

The server distinguishes two client roles (protocol §1.1): one read-write
**commander** and one read-only **observer** per server. `telefacet` is a
**commander-only** client: it connects to the bare `address` from the YAML
(the commander path), owns the camera lifecycle and attributes, and is
refused with HTTP 503 if another commander already holds that server. An
observer (for instance `telefacet-web` in observer mode) may watch the same
server at the same time; the server pushes `state` messages when it comes and
goes, which `telefacet` logs and otherwise ignores. Its streams are
independent of, and yield to, this client's.

## Layout

* `src/net/` — WebSocket client, chunked-frame reassembly, multi-server roster
* `src/data/` — `FrameBufferPool` (recycled byte buffers) and `CameraStore`
  (latest-frame-per-camera, atomic publish/consume)
* `src/client/` — `Client`, the GUI-free API for consuming streams from another
  app (drives the lifecycle into `aruco2x2`, hands out latest per-camera marker
  detections). See `docs/client-api.md`. Part of `telefacet_core`.
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
