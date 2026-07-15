# Stage 5 — ArUco marker support (protocol v6)

Read `PLAN.md` first. Stages 1–4 brought the client to full v5 parity
(protocol, commands, control UX, display overlays). Stage 5 adds **protocol
v6**: the server's new `aruco` / `aruco2x2` save modes stream detected
ArUco/AprilTag markers (`DICT_APRILTAG_16h5`) in a detection block that reuses
the checkerboard block's header fields, discriminated by a new one-byte
`ChunkHeader.detection_kind` (offset 65, carved from `reserved2` — header stays
68 bytes). Source handoff: `docs/aruco-marker-support-handoff.md`. Authoritative
wire spec: `../cherupi-v4l2/docs/websocket-protocol.md` §5.2–5.4.2.

## What changed

**Protocol (`src/net/Protocol.hpp`, `ChunkReassembler.cpp`)**
- `kChunkVersion` 5 → 6.
- `ChunkHeader` gains `detection_kind` (u8 @65); `reserved2` shrinks to
  `u8[2]` @66. `static_assert(sizeof(ChunkHeader) == 68)` still holds.
- New `DetectionKind` enum (`None=0`, `Checkerboard=1`, `Aruco=2`) and
  `MarkerSetHeader` struct (8 bytes: `marker_id` i32, `quadrant` u8, `flags`
  u8, `num_corners` u16), with a `sizeof == 8` assert.
- New `parseMarkerBlock()` mirrors `parseCornerBlock()`. `handleStart()`
  branches on `detection_kind`: checkerboard → `corner_sets`, aruco →
  `aruco_markers`, none → neither. The strict length check
  (`kChunkStartMinSize + corner_block_size`) is unchanged — the marker block is
  what `corner_block_size` measures.

**Data model (`src/data/FrameBufferPool.hpp`, `CameraView.{hpp,cpp}`)**
- New `data::ArucoMarker` (`marker_id`, `quadrant`, `flags`, 4 `corners`) and
  `FrameBuffer::aruco_markers`. Copied off the pooled buffer into
  `CameraView::aruco_markers_` in `uploadIfNew()`, alongside `corner_sets_`.

**Visualization (`src/ui/CameraView.cpp::drawWindow`)**
- After the checkerboard loop, an amber (`IM_COL32(255,160,0,255)`) overlay per
  marker: closed quad (`AddPolyline` + `ImDrawFlags_Closed`), corner dots, and
  a centroid-centered `#id` label (`#id qN` for `aruco2x2`) with a dark shadow.
  Same `sx/sy/img_origin` mapping as checkerboard (full-frame Y-plane pixels).

**Mode selection (`ControlPanel.{hpp,cpp}`, `ConfigLoader.{hpp,cpp}`,
`MultiServerManager.cpp`)**
- `aruco` / `aruco2x2` added to the save-mode selector and the YAML validator.
- Three `aruco_*` params (`full_res_detection`, `num_threads` [aruco2x2 only,
  clamped 1–4], `corner_refine`) editable in the panel and serialized into
  `set_save_mode` / seeded from config, mirroring the `checkerboard_*` path.
- Per-camera `frames_saved` now shows in aruco modes too.

**Tests (`tests/unit_protocol_parse_test.cpp`, new)**
- Byte-level, hardware-free: hand-builds header-only v6 CHUN messages and
  drives `ChunkReassembler` directly. Covers aruco block → `aruco_markers`,
  checkerboard block → `corner_sets` (regression), no-detection, and the
  version gate (accepts 6, rejects 5). All 5 pass.

## Verify
```sh
cmake --build build -j
./build/tests/telefacet_e2e_tests --gtest_filter='ProtocolParse.*'
```
Against live hardware: set the server to `aruco` / `aruco2x2`, put a printed
`DICT_APRILTAG_16h5` marker in view, and confirm the amber quad + id label
track the marker under window resize / letterboxing, with checkerboard modes
unchanged.
