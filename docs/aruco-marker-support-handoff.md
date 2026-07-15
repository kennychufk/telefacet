# Handoff: ArUco marker support (telefacet C++ client)

**Audience:** a fresh Claude Code session working in `/home/kennychufk/workspace/rpiWs/telefacet`.
**Goal:** teach this native OpenGL/ImGui client to (1) speak WebSocket binary **protocol v6**, (2) parse the new **ArUco marker detection block**, (3) **visualize** detected markers on each camera view, and (4) let the user **select the `aruco` / `aruco2x2` save modes** from the control panel.

This mirrors the existing checkerboard support almost 1:1 — wherever you see checkerboard handling, add an ArUco sibling.

> **Authoritative wire spec:** `/home/kennychufk/workspace/rpiWs/cherupi-v4l2/docs/websocket-protocol.md`, §5.2 (version), §5.3 (`ChunkHeader.detection_kind`), §5.4 (detection block), §5.4.2 (`MarkerSetHeader`). Read those sections first. This doc restates the essentials so it is self-contained, but the spec wins if they ever disagree.
>
> **Line numbers below were accurate at handoff time — confirm them with a quick grep/read before editing, as the file may have shifted.**

---

## 0. TL;DR of the server change

The server (cherupi-v4l2) added two save modes, `aruco` and `aruco2x2`, that run `cv::aruco::detectMarkers` (dictionary hard-coded `DICT_APRILTAG_16h5`) best-effort on a worker thread. When a marker is detected, the streamed frame carries a **marker block** in the same WebSocket message as the `ChunkHeader`, exactly where the checkerboard corner block goes today. The binary protocol version bumped **5 → 6**.

Crucially, the marker block **reuses the same header fields** the checkerboard block uses (`corner_block_size`, `num_corner_sets`). A new one-byte `detection_kind` field (carved out of a previously-reserved byte, so **the header is still 68 bytes**) tells you how to parse each record:

| `detection_kind` | meaning | per-record header |
|---|---|---|
| `0` | no detector ran / nothing detected; no block follows | — |
| `1` | checkerboard | `CornerSetHeader` (4 bytes) — **unchanged** |
| `2` | aruco | `MarkerSetHeader` (8 bytes) — **new** |

So: **the checkerboard path stays byte-identical; you branch on `detection_kind` and add a parallel marker path.** The existing strict length check `len == kChunkStartMinSize + corner_block_size` still holds unchanged (the marker block is what `corner_block_size` measures).

---

## 1. Exact wire layout you must implement

### 1.1 Version
`ChunkStartMarker.version` is now **6**. See `src/net/Protocol.hpp:20` (`kChunkVersion = 5u`) → change to `6u`. The version gate is `src/net/ChunkReassembler.cpp:94-98`.

The server is upgraded in lockstep, so **expect v6 only** (don't try to also accept v5 — a v5 checkerboard frame has `detection_kind == 0` because that byte was reserved-zero then, which is ambiguous against "no block". Lockstep avoids the ambiguity).

### 1.2 `ChunkHeader.detection_kind`
The header grew a `detection_kind` byte at **header offset 65** (it took over the first byte of the old `reserved2[3]`, which is now `reserved2[2]`). The header is still **68 bytes**. In `src/net/Protocol.hpp` the tail currently reads:

```cpp
float        lens_position;   // offset 60
std::uint8_t af_state;        // offset 64
std::uint8_t reserved2[3];    // offset 65  (never read today)
```

Change to:

```cpp
float        lens_position;   // offset 60
std::uint8_t af_state;        // offset 64
std::uint8_t detection_kind;  // offset 65 — 0=none, 1=checkerboard, 2=aruco
std::uint8_t reserved2[2];    // offset 66
```

`static_assert(sizeof(ChunkHeader) == 68, ...)` if one exists — it must still hold.

### 1.3 `MarkerSetHeader` (new, 8 bytes) — mirror it in `Protocol.hpp`
One record per detected marker (matches the server's `types.hpp`):

```cpp
struct MarkerSetHeader {
  std::int32_t  marker_id;    // offset 0 — DICT_APRILTAG_16h5 id (0..29)
  std::uint8_t  quadrant;     // offset 4 — 0 for `aruco`; row*2+col (0..3) for `aruco2x2`
  std::uint8_t  flags;        // offset 5 — bit0=1: coords in full-frame Y-plane pixels
  std::uint16_t num_corners;  // offset 6 — always 4
  // followed by num_corners × { float x; float y; }  (8 bytes each)
} __attribute__((packed));
static_assert(sizeof(MarkerSetHeader) == 8);
```

Block layout when `detection_kind == 2`: `num_corner_sets` records, each `MarkerSetHeader` + `4 × {float x, float y}` = **40 bytes/marker**, packed back-to-back, total = `corner_block_size`. The 4 corners are in dictionary canonical order (clockwise from the marker's top-left), in **full-frame Y-plane pixels** — same coordinate space the checkerboard corners already use, so they overlay 1:1 with no extra transform.

---

## 2. Deliverables / task breakdown

1. **Protocol** — accept v6; read `detection_kind`; add `MarkerSetHeader`; parse the marker block.
2. **Data model** — carry parsed markers on `FrameBuffer` and hand them to `CameraView`, exactly like `corner_sets`.
3. **Visualization** — draw, per marker: the 4-corner **quad outline** + a **dot at each corner** + the **marker ID as a centered text label**, in a color **distinct** from the checkerboard green.
4. **Mode selection** — add `aruco` / `aruco2x2` (and params) to the control panel's save-mode UI so the user can enable it.
5. **Tests + docs** — extend protocol tests; update README (`WebSocket protocol v5` → v6) and any relevant `docs/`.

---

## 3. Part A — Protocol parsing

Files: `src/net/Protocol.hpp`, `src/net/ChunkReassembler.cpp`.

1. `Protocol.hpp:20` — `kChunkVersion` 5 → 6.
2. `Protocol.hpp` — add `detection_kind` to `ChunkHeader` (§1.2) and add the `MarkerSetHeader` struct (§1.3). Add a `DetectionKind` enum for readability (`None=0, Checkerboard=1, Aruco=2`).
3. `ChunkReassembler.cpp` — the existing `parseCornerBlock()` is at lines **17-52**, called from `handleStart()` at lines **124-126**. Add a sibling `parseMarkerBlock()` (clone the loop, read `MarkerSetHeader` instead of `CornerSetHeader`, capture `marker_id`/`quadrant`). Then **branch the call site** on `header.detection_kind`:
   - `== Checkerboard` → `parseCornerBlock(...)` (unchanged).
   - `== Aruco` → `parseMarkerBlock(...)`.
   - else → no block.
   Keep the same bounds-checking against `corner_block_size` that `parseCornerBlock` already does.
4. The version check (`ChunkReassembler.cpp:94-98`) and the strict length check (`ChunkReassembler.cpp:119-123`, `kChunkStartMinSize + corner_block_size`) need **no structural change** — the length check already covers the marker block because it reuses `corner_block_size`. Just make sure the version constant is 6.

---

## 4. Part B — Data model

File: `src/data/FrameBufferPool.hpp`.

Today (lines **18-22, 40**):
```cpp
struct CornerSet { std::uint8_t set_id; std::uint8_t flags; std::vector<std::array<float,2>> corners; };
// FrameBuffer:  std::vector<CornerSet> corner_sets;
```

Add a parallel type + field:
```cpp
struct ArucoMarker {
  std::int32_t marker_id;
  std::uint8_t quadrant;
  std::uint8_t flags;
  std::vector<std::array<float,2>> corners;  // 4
};
// FrameBuffer:  std::vector<ArucoMarker> aruco_markers;
```

`parseMarkerBlock` (Part A) fills `FrameBuffer::aruco_markers`. This rides the existing hand-off path unchanged: `ChunkReassembler` → `MultiServerManager::onFrame` (`src/net/MultiServerManager.cpp:165-173`) → `CameraStore::publishFrame` → `CameraView::uploadIfNew`.

In `src/ui/CameraView.cpp::uploadIfNew()`, the corner data is copied off the pooled buffer at **line 87** (before the buffer is released at line 121) into the member `corner_sets_` (declared `src/ui/CameraView.hpp:75`). Add a parallel member `aruco_markers_` and copy it in the same place.

---

## 5. Part C — Visualization

File: `src/ui/CameraView.cpp`, function `drawWindow()`. The checkerboard overlay is drawn with the ImGui window draw-list at **lines 223-247** (the YUV image itself is a shader-rendered texture blitted via `ImGui::Image` at 212-214; overlays are ImGui draw-list calls on top). The scale/offset you need is already computed there:

```cpp
const float sx = disp.x / (float)image_w_;     // display-to-image scale
const float sy = disp.y / (float)image_h_;
// img_origin: top-left of the letterboxed image rect (lines 208-211)
ImDrawList* dl = ImGui::GetWindowDrawList();    // line 217
```

Add an ArUco overlay block right after the checkerboard loop. For each marker in `aruco_markers_`, with `flags & 0x01` set (full-frame coords):

- Map each corner: `ImVec2 p(img_origin.x + c[0]*sx, img_origin.y + c[1]*sy)`.
- **Quad outline:** `dl->AddPolyline` over the 4 mapped corners closed (or 4 `AddLine` calls including last→first). Use a color **distinct from checkerboard green** — e.g. amber `IM_COL32(255, 160, 0, 255)`.
- **Corner dots:** `dl->AddCircleFilled(p, 3.0f, col)` at each corner.
- **ID label:** compute the centroid of the 4 corners, then `dl->AddText(centroid, labelCol, buf)` where `buf` is e.g. `std::to_string(marker_id)` (optionally `"#17"`; for `aruco2x2` you may append the quadrant, e.g. `"#17 q2"`). Consider a small dark outline/background for legibility, or just a bright text color.

Keep the checkerboard overlay untouched — both can render simultaneously in principle (in practice only one save mode is active at a time, so only one of `corner_sets_` / `aruco_markers_` is populated per frame).

Clear `aruco_markers_` on stream stop / black frame wherever `corner_sets_` is cleared, so stale markers don't linger.

---

## 6. Part D — Mode selection (control panel)

The user enables the mode by sending `set_save_mode` with `mode: "aruco"` or `"aruco2x2"` and params:

| param | type | applies | meaning |
|---|---|---|---|
| `aruco_full_res_detection` | bool | both | detect on full-res Y plane vs 2×-subsampled (default `false`, faster) |
| `aruco_num_threads` | int | `aruco2x2` | quadrant parallelism, clamped `[1,4]` (default 4) |
| `aruco_corner_refine` | bool | both | `false`=`CORNER_REFINE_NONE` (fast), `true`=`CORNER_REFINE_SUBPIX` (default `false`) |

**Discovery strategy:** `grep -rn "checkerboard2x2" src/` — every hit is a place that likely needs an `aruco` / `aruco2x2` sibling. The save-mode command is built in `src/ui/ControlPanel.cpp` (and the YAML config is parsed in `src/config/…`). Add:
- The two new modes to the control-panel mode selector (dropdown / radio).
- Input widgets for the three `aruco_*` params (mirror the `checkerboard_*` widgets, shown only when an aruco mode is selected).
- The JSON these produce for `set_save_mode` (mirror how `checkerboard_*` params are serialized).
- If the YAML config supports a `frame_saving.mode`, add the two modes + params to its parser/validation too.

---

## 7. Tests

Protocol/parse tests live under `tests/` (built when `-DTELEFACET_BUILD_TESTS=ON`; see `CMakeLists.txt:84-87`). Add coverage that:
- A v6 `CHUN` start message with a **marker block** (`detection_kind=2`, one or more `MarkerSetHeader` records) parses into `FrameBuffer::aruco_markers` with the right ids/quadrants/corners.
- A v6 **checkerboard** frame (`detection_kind=1`) still parses into `corner_sets` (regression).
- The version gate now accepts 6 and rejects 5 (or whatever policy you land on).

Hand-build the packet bytes the same way the existing protocol tests do. If `Protocol.hpp` gets a new parse source file, register it in the `telefacet_core` source list at `CMakeLists.txt:26-33`.

---

## 8. Docs to update
- `README.md` — it says "WebSocket protocol v5"; bump to v6 and mention aruco modes.
- `docs/upgrade/` contains the staged plan (`PLAN.md`, `stage-1-protocol-v5.md` … `stage-4-display-parity.md`) that documents exactly how checkerboard/v5 was added — **use it as the template** for this work, and consider adding a `stage-*-aruco.md`.

---

## 9. Acceptance criteria
- Client connects to a v6 server and streams frames without "unsupported chunk version" errors.
- With the server in `aruco` or `aruco2x2` mode and a printed `DICT_APRILTAG_16h5` marker in view, each detected marker shows a quad outline + corner dots + its ID label, in the distinct color, correctly aligned on the frame (including under window resize / letterboxing).
- Checkerboard modes still render exactly as before (regression).
- The control panel can switch into `aruco` / `aruco2x2` and set the three params.
- Protocol tests pass.

---

## 10. Gotchas
- **Single block, not two.** Don't add a second trailing block or a second size field. `corner_block_size` / `num_corner_sets` describe whichever block `detection_kind` selects. The length invariant is unchanged.
- **Header is still 68 bytes.** `detection_kind` reused a reserved byte. Keep any `sizeof(ChunkHeader) == 68` assertions.
- **Coordinates are 1:1** with the checkerboard path (full-frame Y-plane pixels) — reuse the exact `sx/sy/img_origin` mapping.
- **Endianness/packing:** the client relies on packed structs + little-endian host; `marker_id` is `int32` — read it signed.
- **quadrant** is informational (which 2×2 sub-frame found the marker); the same physical marker can appear in >1 quadrant record in `aruco2x2`. You don't need to dedupe for visualization.

---

## References (server side)
- Wire spec: `/home/kennychufk/workspace/rpiWs/cherupi-v4l2/docs/websocket-protocol.md` §5.2–5.4.2, §4.5.
- Server structs: `/home/kennychufk/workspace/rpiWs/cherupi-v4l2/src/types.hpp` (`ChunkHeader`, `MarkerSetHeader`, `DetectionKind`).
- Server serialization (reference for exact byte order): `/home/kennychufk/workspace/rpiWs/cherupi-v4l2/src/stream_manager.cpp` `sendChunkHeader()`.
