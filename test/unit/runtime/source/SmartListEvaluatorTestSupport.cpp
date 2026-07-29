// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#include "test/unit/runtime/source/SmartListEvaluatorTestSupport.h"

#include "test/unit/library/TrackTestSupport.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace ao::rt::test
{
  library::test::TrackSpec makeSmartListSpec(std::string_view title,
                                             std::uint16_t year,
                                             std::chrono::milliseconds duration)
  {
    auto spec = library::test::TrackSpec{};
    spec.title = std::string{title};
    spec.year = year;
    spec.duration = duration;
    return spec;
  }
} // namespace ao::rt::test
