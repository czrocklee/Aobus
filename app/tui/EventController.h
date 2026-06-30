// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryController.h"
#include "ShellModel.h"
#include <ao/rt/PlaybackService.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <string>

namespace ao::tui
{
  class EventController final
  {
  public:
    EventController(ftxui::ScreenInteractive& screen,
                    ShellModel& shell,
                    LibraryController& library,
                    rt::PlaybackService& playback);

    std::string const& statusMessage() const noexcept { return _statusMessage; }
    bool handleEvent(ftxui::Event const& event);

  private:
    void openSelectedList();
    void reloadActiveList();
    void applyFilter();
    void toggleDetailPanel();
    void toggleQualityPanel();
    void runCommand(Command const& command);

    ftxui::ScreenInteractive& _screen;
    ShellModel& _shell;
    LibraryController& _library;
    rt::PlaybackService& _playback;
    std::string _statusMessage{"Ready"};
  };
} // namespace ao::tui
