// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <cstdint>
#include <string_view>

namespace ao::audio::test
{
  std::int32_t runAudioFatalProbeScenario(std::string_view scenario);

#ifdef __APPLE__
  // Native Core Audio reentry scenarios, compiled only on Apple hosts. The
  // generic dispatcher routes any "coreaudio-" scenario prefix here; no
  // other scenario name reaches this entry point.
  std::int32_t runCoreAudioFatalProbeScenario(std::string_view scenario);
#endif
} // namespace ao::audio::test
