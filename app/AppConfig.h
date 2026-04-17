// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <string>
#include <utility>

namespace app
{
  constexpr int kDefaultWindowWidth = 989;
  constexpr int kDefaultWindowHeight = 801;
  constexpr int kDefaultPanedPosition = 330;

  struct WindowState final
  {
    int width = kDefaultWindowWidth;
    int height = kDefaultWindowHeight;
    bool maximized = false;
    int panedPosition = kDefaultPanedPosition;
  };

  struct SessionState final
  {
    std::string lastLibraryPath;
  };

  class AppConfig final
  {
  public:
    static AppConfig load();

    void save() const;

    WindowState const& windowState() const { return _windowState; }
    SessionState const& sessionState() const { return _sessionState; }

    void setWindowState(WindowState state) { _windowState = std::move(state); }
    void setSessionState(SessionState state) { _sessionState = std::move(state); }

  private:
    WindowState _windowState;
    SessionState _sessionState;
  };
}
