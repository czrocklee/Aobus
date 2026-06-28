// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/RuntimeTestUtils.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  inline TrackSpec makeSmartListSpec(std::string_view title,
                                     std::uint16_t year,
                                     std::chrono::milliseconds duration = std::chrono::seconds{180})
  {
    auto spec = TrackSpec{};
    spec.title = std::string{title};
    spec.year = year;
    spec.duration = duration;
    return spec;
  }
} // namespace ao::rt::test
