#pragma once

#include <cstdint>

enum class HeaderType : uint32_t { ack, bayer };
struct ResponseHeader {
  HeaderType header_type;
  uint32_t camera_id;
  uint32_t payload_size;
  uint32_t width;
  uint32_t height;
  uint32_t stride;
  uint32_t frame_id;
};
