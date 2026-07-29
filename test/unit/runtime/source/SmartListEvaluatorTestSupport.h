// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "test/unit/library/TrackTestSupport.h"

#include <chrono>
#include <cstdint>
#include <string_view>

namespace ao::rt::test
{
  library::test::TrackSpec makeSmartListSpec(std::string_view title,
                                             std::uint16_t year,
                                             std::chrono::milliseconds duration = std::chrono::seconds{180});
} // namespace ao::rt::test
