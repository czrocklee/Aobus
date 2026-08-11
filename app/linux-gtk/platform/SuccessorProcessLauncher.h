// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/desktop/LibrarySwitch.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::gtk
{
  struct SuccessorLaunchPlan final
  {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::optional<std::string> optActivationToken{};

    friend bool operator==(SuccessorLaunchPlan const&, SuccessorLaunchPlan const&) = default;
  };

  Result<SuccessorLaunchPlan> planSuccessorLaunch(
    desktop::LibrarySwitchRequest const& request,
    std::optional<std::string_view> optActivationToken = std::nullopt,
    std::optional<std::filesystem::path> optAppImageExecutable = std::nullopt);

  Result<> launchDetachedSuccessor(SuccessorLaunchPlan const& plan);

  Result<> launchDetachedSuccessor(desktop::LibrarySwitchRequest const& request,
                                   std::optional<std::string_view> optActivationToken = std::nullopt);
} // namespace ao::gtk
