// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ao::desktop
{
  enum class DetachedProcessStandardStreams : std::uint8_t
  {
    NativeDefault,
    InheritParent,
  };

  struct DetachedProcessLaunch final
  {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::optional<std::vector<std::string>> optEnvironment{};
    DetachedProcessStandardStreams standardStreams = DetachedProcessStandardStreams::NativeDefault;
  };

  /** Create and detach a child without retaining a process-lifetime owner. */
  Result<> launchDetachedProcess(DetachedProcessLaunch const& launch);
} // namespace ao::desktop
