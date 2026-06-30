// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryController.h"
#include "OutputDeviceController.h"
#include "PlaybackPanel.h"
#include "ShellModel.h"
#include <ao/rt/PlaybackService.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/box.hpp>

#include <string>
#include <vector>

namespace ao::tui
{
  class EventController final
  {
  public:
    EventController(ftxui::ScreenInteractive& screen,
                    ShellModel& shell,
                    LibraryController& library,
                    rt::PlaybackService& playback,
                    OutputDeviceController* outputDevices = nullptr,
                    ftxui::Box* outputDeviceButtonBox = nullptr,
                    std::vector<OutputDeviceRowBox>* outputDeviceRowBoxes = nullptr,
                    ftxui::Box* libraryButtonBox = nullptr,
                    ftxui::Box* qualityButtonBox = nullptr);

    std::string const& statusMessage() const noexcept { return _statusMessage; }
    bool handleEvent(ftxui::Event const& event);

  private:
    void openSelectedList();
    void reloadActiveList();
    void applyFilter();
    void toggleListChooser();
    void toggleDetailPanel();
    void toggleQualityPanel();
    void toggleOutputDevices();
    void selectOutputDevice();
    void revealCurrentTrack();
    void runCommand(Command const& command);
    bool handleMouse(ftxui::Mouse const& mouse);

    ftxui::ScreenInteractive& _screen;
    ShellModel& _shell;
    LibraryController& _library;
    rt::PlaybackService& _playback;
    OutputDeviceController* _outputDevices = nullptr;
    ftxui::Box* _outputDeviceButtonBox = nullptr;
    std::vector<OutputDeviceRowBox>* _outputDeviceRowBoxes = nullptr;
    ftxui::Box* _libraryButtonBox = nullptr;
    ftxui::Box* _qualityButtonBox = nullptr;
    std::string _statusMessage{"Ready"};
  };
} // namespace ao::tui
