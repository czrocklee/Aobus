// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/audio/Backend.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ao::rt
{
  struct PlaybackState;
}

namespace ao::gtk
{
  struct OutputSelectorRow final
  {
    enum class Kind : std::uint8_t
    {
      BackendHeader,
      DeviceProfile,
    };

    Kind kind{};
    std::string title;
    audio::BackendId backendId;
    audio::DeviceId deviceId;
    audio::ProfileId profileId;
    bool active = false;
  };

  std::vector<OutputSelectorRow> buildOutputSelectorRows(rt::PlaybackState const& state);
} // namespace ao::gtk
