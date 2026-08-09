// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::audio::test
{
  std::int32_t runAudioFatalProbeScenario(std::string_view scenario);
} // namespace ao::audio::test
