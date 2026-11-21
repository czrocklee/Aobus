// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <optional>
#include <string_view>

namespace ao::audio
{
  inline constexpr auto kPipeWireNodePrefix = std::string_view{"node:"};

  // A saved key names a logical sink; only the live connection may resolve it to an object serial.
  constexpr std::optional<std::string_view> pipeWireTargetNodeName(std::string_view deviceId) noexcept
  {
    if (!deviceId.starts_with(kPipeWireNodePrefix))
    {
      return std::nullopt;
    }

    deviceId.remove_prefix(kPipeWireNodePrefix.size());
    return deviceId.empty() || deviceId.contains('\0') ? std::nullopt : std::optional{deviceId};
  }
} // namespace ao::audio
