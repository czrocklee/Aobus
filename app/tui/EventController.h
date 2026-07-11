// SPDX-License-Identifier: MIT
// Copyright (c) 2024-2026 Aobus Contributors

#pragma once

#include "LibraryController.h"
#include "OutputDeviceController.h"
#include "ShellInteractionModel.h"
#include "TrackTable.h"
#include "TuiHitRegions.h"
#include <ao/rt/NotificationState.h>
#include <ao/rt/PlaybackService.h>
#include <ao/rt/TrackField.h>
#include <ao/rt/completion/CompletionResult.h>
#include <ao/uimodel/playback/seek/SeekSliderInteractionModel.h>
#include <ao/uimodel/status/activity/ActivityStatusViewModel.h>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ao::rt
{
  class AppRuntime;
  class NotificationService;
  class PlaybackSequenceService;
}

namespace ao::tui
{
  using CommandCompletionCallback = std::function<std::optional<rt::CompletionResult>(std::string_view draft)>;

  struct EventControllerBindings final
  {
    OutputDeviceController* outputDevices = nullptr;
    TuiHitRegions* hitRegions = nullptr;
    std::vector<TrackColumnWidthOverride>* trackColumnWidthOverrides = nullptr;
    uimodel::ActivityStatusViewModel* activityStatusViewModel = nullptr;
    rt::NotificationService* notifications = nullptr;
    CommandCompletionCallback commandCompletionCallback{};
  };

  class EventController final
  {
  public:
    EventController(ftxui::ScreenInteractive& screen,
                    ShellInteractionModel& shell,
                    LibraryController& library,
                    rt::AppRuntime& runtime,
                    EventControllerBindings bindings = {});

    bool isQualityHoverVisible() const noexcept { return _qualityHoverVisible; }
    HoveredButton hoveredButton() const noexcept { return _hoveredButton; }
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
    void toggleNotificationCenter();
    void selectOutputDevice();
    void selectPresentation();
    void revealCurrentTrack();
    void togglePlaybackFromSelection();
    void runCommand(Command const& command);
    void postActivityNotification(rt::NotificationSeverity severity, std::string message);
    void refreshCommandCompletion();
    bool handleMouse(ftxui::Mouse const& mouse);
    bool selectTrackFromScrollbar(std::int32_t row);
    void syncSeekSlider();
    std::chrono::milliseconds seekRailElapsed(std::int32_t column) const;
    void applySeekDecision(uimodel::SeekSliderDecision const& decision);
    void cancelSeekInteraction();

    struct TrackColumnResizeDrag final
    {
      rt::TrackField field = rt::TrackField::Title;
      std::int32_t startX = 0;
      std::int32_t startColumns = 0;
    };

    struct TrackScrollbarDrag final
    {};

    struct SeekRailDrag final
    {};

    ftxui::ScreenInteractive& _screen;
    ShellInteractionModel& _shell;
    LibraryController& _library;
    rt::PlaybackService& _playback;
    rt::PlaybackSequenceService& _playbackSequence;
    OutputDeviceController* _outputDevices = nullptr;
    TuiHitRegions* _hitRegions = nullptr;
    std::vector<TrackColumnWidthOverride>* _trackColumnWidthOverrides = nullptr;
    std::optional<TrackColumnResizeDrag> _optTrackColumnResizeDrag{};
    std::optional<TrackScrollbarDrag> _optTrackScrollbarDrag{};
    std::optional<SeekRailDrag> _optSeekRailDrag{};
    uimodel::SeekSliderInteractionModel _seekSlider{};
    uimodel::ActivityStatusViewModel* _activityStatusViewModel = nullptr;
    rt::NotificationService* _notifications = nullptr;
    CommandCompletionCallback _commandCompletionCallback{};
    bool _qualityHoverVisible = false;
    HoveredButton _hoveredButton = HoveredButton::None;
  };
} // namespace ao::tui
