#pragma once

#include <spdlog/spdlog.h>

namespace telefacet::log {

inline void init() {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
  spdlog::set_level(spdlog::level::info);
}

}  // namespace telefacet::log
