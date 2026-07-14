# Stage 1 handoff — Protocol v5 core (P0)

**Goal:** make the C++ client render live frames from the current
`cherupi-v4l2` server again. Read `PLAN.md` in this folder first for full
context. This stage touches the wire/data layer only — **no new UI or
controls**; new header fields are parsed and plumbed but not yet displayed.

## Why nothing renders today
`src/net/ChunkReassembler.cpp:40` requires the start message to be exactly 44
bytes (`kChunkStartTotalSize`) and `:50` requires `version == 2`. The v5 server
sends a 76-byte start message (8-byte marker + 68-byte header) plus an optional
`CornerBlock`, version 5 → every frame is dropped before any chunk arrives.

## Authoritative spec
`../cherupi-v4l2/docs/websocket-protocol.md` §5.2–5.5 (structs) and §8 (history).
Cross-check field offsets against `../cherupi-v4l2/src/types.hpp` (packed).
Reference impl to port: `../telefacet-web/src/services/WebSocketManager.js`
(`handleChunkStart`, `parseCornerBlock` — lines ~188–298).

## v5 `ChunkHeader` (68 bytes) — offsets
`frame_uuid`0 `frame_id`4 `camera_id`8 `total_chunks`12 `total_size`16
`bytes_per_line`20 `width`24 `height`28 `pixel_format`32 `frames_saved`36
`timestamp_us`(u64)40 `frame_duration_us`48 `corner_block_size`52
`num_corner_sets`(u16)56 `reserved`(u16)58 `lens_position`(float)60
`af_state`(u8)64 `reserved2`(u8[3])65. Start message length =
`8 + 68 + corner_block_size` = `76 + corner_block_size`.

`CornerBlock` (§5.4, present iff `num_corner_sets > 0`): `num_corner_sets` ×
[`CornerSetHeader`(4B: `set_id`u8, `flags`u8, `num_corners`u16) then
`num_corners` × {float x, float y}]. Coords are full-frame Y-plane pixels.

## Tasks
1. `src/net/Protocol.hpp`
   - Bump `kChunkVersion` to `5`.
   - Replace `ChunkHeader` with the 68-byte layout above (keep first 40 bytes identical — they already match).
   - Add `kChunkStartMinSize = sizeof(ChunkStartMarker) + sizeof(ChunkHeader)` (76); note the message may be longer by `corner_block_size`.
2. `src/data/FrameBufferPool.hpp` — extend `FrameBuffer`: `uint64_t timestamp_us`, `uint32_t frame_duration_us`, `float lens_position`, `uint8_t af_state`, and a corner-set representation (e.g. `std::vector<CornerSet>` where `CornerSet { uint8_t set_id; uint8_t flags; std::vector<std::array<float,2>> corners; }`). Reset these on pool reuse/acquire.
3. `src/net/ChunkReassembler.cpp` `handleStart`
   - Accept `len >= kChunkStartMinSize` and require `len == kChunkStartMinSize + corner_block_size`.
   - Version check → 5.
   - Copy the new packed fields to locals (packed-struct ref rule already used here) and populate `FrameBuffer` on both the header-only path and the multi-chunk path.
   - Parse the `CornerBlock` (bounds-checked, mirror `parseCornerBlock`) into the buffer's corner sets.
4. README + `configs/example.yaml` — drop "protocol v2"/"10-bit SRGGB" wording; note v5 / YUV420. Leave `crop_*` config alone (Stage 2 removes it).

## Watch out for
- Packed structs: copy fields to locals before passing to `spdlog`/functions (existing code already does this — keep it).
- Header-only frames (`total_chunks==0 && total_size==0`) still carry a valid v5 header (and possibly a corner block); handle new fields there too.
- Little-endian host assumption is fine (existing).
- Don't wire corners/lens/af into `CameraStore`/UI yet — just carry them on `FrameBuffer`. Display is Stage 3/4.

## Done when
- Build clean (GUI + `-DTELEFACET_BUILD_TESTS=ON`).
- `./build/telefacet configs/<v5-server>.yaml` renders live frames; no "invalid chunk start size" / "unsupported chunk version" spam.
- e2e suite in `tests/` passes against a v5 server.

Then write `stage-2-commands-config.md` and update `PLAN.md` Status.
