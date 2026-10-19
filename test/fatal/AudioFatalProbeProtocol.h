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

#ifdef __APPLE__
  // Apple-only native Core Audio reentry expectations. The all-host probe
  // runner keeps iterating audioFatalProbeExpectations() only; the Core
  // executable's CoreAudioBackendFatalProbeTest drives this span through the
  // same ao_audio_fatal_probe child entry.
  std::span<AudioFatalProbeExpectation const> coreAudioFatalProbeExpectations() noexcept;
#endif
} // namespace ao::audio::test
