// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <span>
#include <string_view>

namespace ao::audio::test
{
  struct AudioFatalProbeExpectation final
  {
    std::string_view scenario;
    std::string_view category;
    std::string_view context;
  };

  std::span<AudioFatalProbeExpectation const> audioFatalProbeExpectations() noexcept;
} // namespace ao::audio::test
