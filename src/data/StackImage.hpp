#pragma once

// CPU helpers for a stacked raw image (protocol §5.5, pixel format 'SF32'):
// width × height little-endian float32, linear DN above black, one sample per
// 2×2 Bayer quad, rows of bytes_per_line bytes. Mirrors telefacet-web's
// webgl/stackImage.js (display) and services/npy.js (saving).

#include <cstddef>
#include <cstdint>
#include <vector>

#include "data/FrameBufferPool.hpp"

namespace telefacet::data {

// Where the board's white (the 99.5th percentile, the server's
// `white_fraction`) lands on the display scale.
inline constexpr float kStackWhitePoint = 0.9f;

// Render an SF32 frame to 8-bit gray for display, tightly packed (width ×
// height). Its absolute level depends on the exposure and the sensor's bit
// depth, so it is stretched: the 99.5th percentile maps to kStackWhitePoint,
// then gamma 2.2 makes it look like a photograph rather than a linear dump.
// Returns the DN mapped to full white; 0 (and an empty `gray`) when the frame
// carries no complete SF32 payload.
float renderStackGray(const FrameBuffer& f, std::vector<std::uint8_t>& gray);

// The .npy (format 1.0) header for a C-order little-endian float32 array of
// shape (rows, cols), padded so the data starts on a 64-byte boundary.
std::vector<std::uint8_t> npyHeader(std::size_t rows, std::size_t cols);

// A complete .npy file for an SF32 frame, row padding dropped, so `np.load`
// gives back a (height, width) float32 image. Empty when the payload is short.
std::vector<std::uint8_t> stackImageNpy(const FrameBuffer& f);

}  // namespace telefacet::data
