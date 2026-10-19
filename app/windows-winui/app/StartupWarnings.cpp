// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Aobus Contributors

#include <ao/winui/app/StartupWarnings.h>

#include <optional>
#include <string>
#include <utility>

namespace ao::winui
{
  std::string StartupWarnings::add(std::string warning)
  {
    _warnings.push_back(std::move(warning));
    return statusLine();
  }

  std::string StartupWarnings::statusLine() const
  {
    auto line = std::string{};

    for (auto const& warning : _warnings)
    {
      if (!line.empty())
      {
        line.push_back(' ');
      }

      line.append(warning);
    }

    return line;
  }

  std::optional<std::string> StartupWarnings::settle(bool const scansAtOpen, bool const scanFailed)
  {
    auto line = statusLine();
    _warnings.clear();

    if (!scansAtOpen || scanFailed || line.empty())
    {
      return std::nullopt;
    }

    return line;
  }
} // namespace ao::winui
