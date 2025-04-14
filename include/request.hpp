#pragma once

#include <cstdint>

enum class Command : uint32_t { configure, control, start, stop };
struct Request {
  Command command;
  uint32_t key;
  uint8_t value[32];
};
