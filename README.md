# telefacet

Native C++ / OpenGL replacement for the `telefacet-web` Vue client. Connects
to one or more `cherupi-v4l2` WebSocket servers (WebSocket protocol v8),
converts the YUV420 frames delivered by the server's hardware ISP to RGB on
the GPU, and renders them in a dockable multi-camera view. Detector process modes
(`checkerboard` / `checkerboard2x2` / `aruco` / `aruco2x2`) overlay their
per-frame detections on each camera view — checkerboard corners in green,
ArUco/AprilTag markers as amber quads with corner dots and id labels.

### Captures

Two on-demand captures work in **any** process mode and get that mode's
detection (protocol v8, §4.17, §4.19):

* **Trigger** takes the next frame of every running camera — the shutter a
  calibration rig fires once its arm has stopped.
* **Stack** averages N raw Bayer frames per camera into one half-resolution
  float image, so corner scatter falls as 1/√N. It needs `raw: true` on the
  server in the YAML (the section appears only then); the stack's exposure and
  gain can be pinned, and each camera's result line shows how bright the
  board's white came out.

The server saves neither: it sends the frames back to this client. Firing a
capture switches the tiles to the **Capture** view (toggle with **V**), where
each camera shows its last capture with a badge (kind, id, frame, age, saved
state) and its own detections. **Capture overlay** draws the last capture's
corners over the live frames in cyan. Every capture is written to this
machine (unless **Save captures** is off) under
`<captures.dir>/<YYYYMMDD-HHMMSS>-captures/<host_port>/` — triggers as
`cam<cam>-<frame_id>.yuv`, stacks as `.npy` (float32, `np.load`-able), each
with a `.json` sidecar holding the metadata, the detection and the server's
result entry. Headlessly, `Client::triggerCapture()` and
`Client::stackCapture()` return the frames instead (`docs/client-api.md`).

Hovering a camera view shows that frame's lens position (dioptres) and AF
state. The **Focus FoM** checkbox (Options, off by default) adds each view's
libcamera `FocusFoM` sharpness score with a curve of its last 80 values,
centred on the frame, and outlines on hover the fixed central region the
server's IPA averages it over.
It works in header-only mode too, so a focus sweep can be watched without
streaming pixels.

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
streams. Press **P** to hide/show the control panel, **V** to switch the
tiles between live frames and the last captures.

### Loading another config

Another YAML can be loaded without restarting: type its path in the panel's
**Config** section and press **Load** (or Enter), or drop the file on the
window. That is how the same Pi moves between sensors, e.g. from an IMX477
config to an IMX708 one. The new file is read first (a bad one is reported
there and the current session kept), then the current connections are closed
for good and the new config's opened. The server has one commander slot, so
the old connection is gone before the new one asks for it. The new
connection's `discover` names its sensor: a server whose sensor changes
releases its cameras (configure and start again), and one whose sensor stays
keeps them running, so you only reopen the streams. A server that refuses the
connection because another commander holds it shows
`[refused: another commander]` under **Servers**.

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
* `src/data/` — `FrameBufferPool` (recycled byte buffers), `CameraStore`
  (latest-frame-per-camera, atomic publish/consume, last capture per camera),
  `CaptureSaver` (writes captures on a worker thread) and `StackImage`
  (stacked-image display and `.npy` helpers)
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
  behavior of the JS client). Capture frames bypass this: each one is kept as
  its camera's last capture, handed to the `CaptureSaver` from the network
  thread and written on the saver's own thread.
