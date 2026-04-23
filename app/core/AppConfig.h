// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2025 RockStudio Contributors

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace app::core
{
  constexpr std::int32_t kDefaultWindowWidth = 989;
  constexpr std::int32_t kDefaultWindowHeight = 801;
  constexpr std::int32_t kDefaultPanedPosition = 330;

  struct WindowState final
  {
    std::int32_t width = kDefaultWindowWidth;
    std::int32_t height = kDefaultWindowHeight;
    bool maximized = false;
    std::int32_t panedPosition = kDefaultPanedPosition;
  };

  struct SessionState final
  {
    std::string lastLibraryPath;
  };

  struct TrackViewState final
  {
    std::vector<std::string> columnOrder;
    std::vector<std::string> hiddenColumns;
    std::map<std::string, int, std::less<>> columnWidths;
  };

  class AppConfig final
  {
  public:
    void save() const;

    WindowState const& windowState() const { return _windowState; }
    SessionState const& sessionState() const { return _sessionState; }
    TrackViewState const& trackViewState() const { return _trackViewState; }

    void setWindowState(WindowState state) { _windowState = std::move(state); }
    void setSessionState(SessionState state) { _sessionState = std::move(state); }
    void setTrackViewState(TrackViewState state) { _trackViewState = std::move(state); }

    static AppConfig load();

  private:
    WindowState _windowState;
    SessionState _sessionState;
    TrackViewState _trackViewState;
  };
}
