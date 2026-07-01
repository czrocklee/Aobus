// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryController.h"
#include "OutputDeviceController.h"
#include "PlaybackPanel.h"
#include "Render.h"
#include "ShellModel.h"
#include "TrackTable.h"
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionResult.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/screen/box.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::tui
{
  using CommandCompletionCallback = std::function<std::optional<rt::CompletionResult>(std::string_view draft)>;

  struct EventControllerBindings final
  {
    OutputDeviceController* outputDevices = nullptr;
    ftxui::Box* outputDeviceButtonBox = nullptr;
    std::vector<OutputDeviceRowBox>* outputDeviceRowBoxes = nullptr;
    ftxui::Box* libraryButtonBox = nullptr;
    ftxui::Box* qualityButtonBox = nullptr;
    ftxui::Box* presentationButtonBox = nullptr;
    std::vector<PresentationRowBox>* presentationRowBoxes = nullptr;
    std::vector<TrackColumnResizeHandle>* trackColumnResizeHandles = nullptr;
    std::vector<TrackColumnWidthOverride>* trackColumnWidthOverrides = nullptr;
    ftxui::Box* trackTableBox = nullptr;
    std::vector<TrackSectionRowBox>* trackSectionRowBoxes = nullptr;
    CommandCompletionCallback commandCompletionCallback{};
  };

  class EventController final
  {
  public:
    EventController(ftxui::ScreenInteractive& screen,
                    ShellModel& shell,
                    LibraryController& library,
                    rt::PlaybackService& playback,
                    EventControllerBindings bindings = {});

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
    void togglePresentationPanel();
    void selectOutputDevice();
    void selectPresentation();
    void revealCurrentTrack();
    void runCommand(Command const& command);
    void refreshCommandCompletion();
    bool handleMouse(ftxui::Mouse const& mouse);
    bool selectTrackFromScrollbar(std::int32_t row);

    struct TrackColumnResizeDrag final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::int32_t startX = 0;
      std::int32_t startColumns = 0;
    };

    struct TrackScrollbarDrag final
    {};

    ftxui::ScreenInteractive& _screen;
    ShellModel& _shell;
    LibraryController& _library;
    rt::PlaybackService& _playback;
    OutputDeviceController* _outputDevices = nullptr;
    ftxui::Box* _outputDeviceButtonBox = nullptr;
    std::vector<OutputDeviceRowBox>* _outputDeviceRowBoxes = nullptr;
    ftxui::Box* _libraryButtonBox = nullptr;
    ftxui::Box* _qualityButtonBox = nullptr;
    ftxui::Box* _presentationButtonBox = nullptr;
    std::vector<PresentationRowBox>* _presentationRowBoxes = nullptr;
    std::vector<TrackColumnResizeHandle>* _trackColumnResizeHandles = nullptr;
    std::vector<TrackColumnWidthOverride>* _trackColumnWidthOverrides = nullptr;
    ftxui::Box* _trackTableBox = nullptr;
    std::vector<TrackSectionRowBox>* _trackSectionRowBoxes = nullptr;
    std::optional<TrackColumnResizeDrag> _optTrackColumnResizeDrag{};
    std::optional<TrackScrollbarDrag> _optTrackScrollbarDrag{};
    CommandCompletionCallback _commandCompletionCallback{};
    std::string _statusMessage{"Ready"};
  };
} // namespace ao::tui
