#include "data/StackImage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

namespace telefacet::data {

namespace {

constexpr float       kGamma      = 2.2f;
constexpr int         kLutSize    = 4096;
constexpr std::size_t kMaxSamples = std::size_t{1} << 18;

// Whether `f` holds a full SF32 image of its declared geometry.
bool hasSf32(const FrameBuffer& f) {
  return f.width > 0 && f.height > 0 && f.bytes_per_line >= f.width * 4 &&
         f.data.size() >= static_cast<std::size_t>(f.bytes_per_line) * f.height;
}

float sample(const FrameBuffer& f, std::size_t x, std::size_t y) {
  float v;
  std::memcpy(&v, f.data.data() + y * f.bytes_per_line + x * 4, sizeof(v));
  return v;
}

}  // namespace

float renderStackGray(const FrameBuffer& f, std::vector<std::uint8_t>& gray) {
  gray.clear();
  if (!hasSf32(f)) return 0.0f;
  const std::size_t w = f.width, h = f.height, n = w * h;

  // The 99.5th percentile of a strided sample (at most kMaxSamples values,
  // which pins it to well under 1 % on a 3 MP image).
  const std::size_t step = std::max<std::size_t>(1, n / kMaxSamples);
  std::vector<float> s;
  s.reserve(n / step + 1);
  for (std::size_t i = 0; i < n; i += step) {
    const float v = sample(f, i % w, i / w);
    s.push_back(std::isfinite(v) ? v : 0.0f);
  }
  const std::size_t k =
      std::min(s.size() - 1, static_cast<std::size_t>(0.995 * (s.size() - 1)));
  std::nth_element(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(k),
                   s.end());
  const float p995  = s[k];
  const float white = p995 > 0.0f ? p995 / kStackWhitePoint : 1.0f;

  std::array<std::uint8_t, kLutSize> lut{};
  for (int i = 0; i < kLutSize; ++i)
    lut[i] = static_cast<std::uint8_t>(std::lround(
        255.0 * std::pow(static_cast<double>(i) / (kLutSize - 1), 1.0 / kGamma)));
  const float to_index = (kLutSize - 1) / white;

  gray.resize(n);
  for (std::size_t y = 0; y < h; ++y) {
    for (std::size_t x = 0; x < w; ++x) {
      const float i = sample(f, x, y) * to_index;
      // NaN and negatives (noise below black) go to 0.
      gray[y * w + x] =
          i > 0.0f ? lut[i < kLutSize - 1 ? static_cast<int>(i) : kLutSize - 1]
                   : 0;
    }
  }
  return white;
}

std::vector<std::uint8_t> npyHeader(std::size_t rows, std::size_t cols) {
  const std::string dict = "{'descr': '<f4', 'fortran_order': False, 'shape': (" +
                           std::to_string(rows) + ", " + std::to_string(cols) +
                           "), }";
  const std::size_t preamble = 6 + 2 + 2;  // magic, version, header length
  const std::size_t total = (preamble + dict.size() + 1 + 63) / 64 * 64;
  std::string text = dict;
  text.resize(total - preamble - 1, ' ');
  text += '\n';

  std::vector<std::uint8_t> out = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
  const auto len = static_cast<std::uint16_t>(text.size());
  out.push_back(static_cast<std::uint8_t>(len & 0xFF));
  out.push_back(static_cast<std::uint8_t>(len >> 8));
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

std::vector<std::uint8_t> stackImageNpy(const FrameBuffer& f) {
  if (!hasSf32(f)) return {};
  std::vector<std::uint8_t> out = npyHeader(f.height, f.width);
  const std::size_t row = static_cast<std::size_t>(f.width) * 4;
  const std::size_t hdr = out.size();
  out.resize(hdr + row * f.height);
  for (std::size_t y = 0; y < f.height; ++y)
    std::memcpy(out.data() + hdr + y * row,
                f.data.data() + y * f.bytes_per_line, row);
  return out;
}

}  // namespace telefacet::data
