// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/fleet/Model.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ao::fleet
{
  struct RouteStatistics
  {
    std::string route;
    std::size_t usable = 0;
    std::size_t unusable = 0;
    bool paused = false;
  };

  class RouteStore final
  {
  public:
    explicit RouteStore(std::filesystem::path out);

    Result<> record(std::string_view phaseId, ReviewVerdict verdict, std::string_view reason);
    Result<std::vector<RouteStatistics>> statistics(std::size_t window, bool* trailingCorruption = nullptr) const;
    Result<bool> paused(std::string_view route) const;
    Result<> reset(std::string_view route);

  private:
    Result<std::string> latestReset(std::string_view route) const;

    std::filesystem::path _out;
  };
} // namespace ao::fleet
