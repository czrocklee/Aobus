// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/winui/Theme.h>

#include <filesystem>

namespace ao::winui
{
  class ThemeCoordinator final
  {
  public:
    explicit ThemeCoordinator(std::filesystem::path themePath);

    Result<Theme> reload();
    Theme const& theme() const noexcept { return _session.theme(); }
    std::filesystem::path const& path() const noexcept { return _themePath; }

  private:
    std::filesystem::path _themePath;
    ThemeSessionModel _session;
  };
} // namespace ao::winui
