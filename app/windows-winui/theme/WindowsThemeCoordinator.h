// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include <ao/Error.h>
#include <ao/uimodel/preference/WindowsTheme.h>

#include <filesystem>

namespace ao::winui
{
  class WindowsThemeCoordinator final
  {
  public:
    explicit WindowsThemeCoordinator(std::filesystem::path themePath);

    Result<uimodel::WindowsTheme> reload();
    uimodel::WindowsTheme const& theme() const noexcept { return _session.theme(); }
    std::filesystem::path const& path() const noexcept { return _themePath; }

  private:
    std::filesystem::path _themePath;
    uimodel::WindowsThemeSessionModel _session;
  };
} // namespace ao::winui
